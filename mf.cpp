#include <algorithm>
#include <condition_variable>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <queue>
#include <thread>
#include <unordered_set>
#include <random>
#include <cstring>
#include <fstream>
#include <vector>
#include <new>
#include <string>
#include <memory>
#include <set>

#include "mf.h"

#if defined USESSE
#include <pmmintrin.h>
#endif

#if defined USEAVX
#include <immintrin.h>
#endif

#if defined USEOMP
#include <omp.h>
#endif

namespace mf
{

using namespace std;

namespace // unnamed namespace
{

mf_int const kALIGNByte = 32;
mf_int const kALIGN = kALIGNByte/sizeof(mf_float);

vector<string> tokenize(string str)
{
    vector<string> tokens;

    if(str.empty())
        return tokens;

    basic_string<char>::size_type begin = 0, end = 0;
    while((end = str.find_first_of(" ", begin)) != string::npos)
    {
        string token = str.substr(begin, end-begin);
        if(token.size() != 0)
            tokens.push_back(token);
        begin = end + 1;
    }
    string tail = str.substr(begin);
    if(tail.size() != 0)
        tokens.push_back(tail);

    return tokens;
}

class Scheduler
{
public:
    Scheduler(mf_int nr_bins, mf_int nr_threads, vector<mf_int> cv_blocks);
    mf_int get_job();
    void put_job(mf_int block, mf_double loss);
    mf_double get_loss();
    void wait_for_jobs_done();
    void resume();
    void terminate();
    bool is_terminated();

private:
    mf_int nr_bins;
    mf_int nr_threads;
    mf_int nr_done_jobs;
    mf_int target;
    mf_int nr_paused_threads;
    bool terminated;
    vector<mf_int> counts;
    vector<mf_int> busy_p_blocks;
    vector<mf_int> busy_q_blocks;
    vector<mf_double> block_losses;
    unordered_set<mf_int> cv_blocks;
    mutex mtx;
    condition_variable cond_var;
    default_random_engine generator;
    uniform_real_distribution<mf_float> distribution;
    priority_queue<pair<mf_float, mf_int>, 
                   vector<pair<mf_float, mf_int>>, 
                   greater<pair<mf_float, mf_int>>> pq;
};

Scheduler::Scheduler(mf_int nr_bins, mf_int nr_threads, vector<mf_int> cv_blocks)
    : nr_bins(nr_bins), 
      nr_threads(nr_threads), 
      nr_done_jobs(0), 
      target(nr_bins*nr_bins),
      nr_paused_threads(0), 
      terminated(false), 
      counts(nr_bins*nr_bins, 0), 
      busy_p_blocks(nr_bins, 0), 
      busy_q_blocks(nr_bins, 0), 
      cv_blocks(cv_blocks.begin(), cv_blocks.end()),
      block_losses(nr_bins*nr_bins, 0),
      distribution(0.0, 1.0) 
{
    for(mf_int i = 0; i < nr_bins*nr_bins; i++)
        if(this->cv_blocks.find(i) == this->cv_blocks.end())
            pq.emplace(distribution(generator), i);
}

mf_int Scheduler::get_job()
{
    lock_guard<mutex> lock(mtx);
    vector<pair<mf_float, mf_int>> locked_blocks;

    while(true) 
    {
        pair<mf_float, mf_int> block = pq.top();
        pq.pop();
        mf_int p_block = block.second/nr_bins;
        mf_int q_block = block.second%nr_bins;
        if(busy_p_blocks[p_block] || busy_q_blocks[q_block])
        {
            locked_blocks.push_back(block);
            continue;
        }
        for(auto &block : locked_blocks)
            pq.push(block);
        busy_p_blocks[p_block] = 1;
        busy_q_blocks[q_block] = 1;
        counts[block.second]++;
        return block.second;
    }
}

void Scheduler::put_job(mf_int block_idx, mf_double loss)
{
    {
        lock_guard<mutex> lock(mtx);
        busy_p_blocks[block_idx/nr_bins] = 0;
        busy_q_blocks[block_idx%nr_bins] = 0;
        block_losses[block_idx] = loss;
        nr_done_jobs++;
        mf_float priority = (mf_float)counts[block_idx]+distribution(generator);
        pq.emplace(priority, block_idx);
        nr_paused_threads++;
        cond_var.notify_all();
    }

    {
        unique_lock<mutex> lock(mtx);
        cond_var.wait(lock, [&] {
            return nr_done_jobs < target;
        });
    }

    {
        lock_guard<mutex> lock(mtx);
        --nr_paused_threads;
    }
}

mf_double Scheduler::get_loss() 
{
    lock_guard<mutex> lock(mtx);
    return accumulate(block_losses.begin(), block_losses.end(), 0.0);
}

void Scheduler::wait_for_jobs_done()
{
    unique_lock<mutex> lock(mtx);

    cond_var.wait(lock, [&] {
        return nr_done_jobs >= target;
    });

    cond_var.wait(lock, [&] {
        return nr_paused_threads == nr_threads;
    });
}

void Scheduler::resume()
{
    lock_guard<mutex> lock(mtx);
    target += nr_bins*nr_bins;
    cond_var.notify_all();
}

void Scheduler::terminate()
{
    lock_guard<mutex> lock(mtx);
    terminated = true;
}

bool Scheduler::is_terminated() 
{
    lock_guard<mutex> lock(mtx);
    return terminated;
}

#if defined USESSE
inline void sg_update(
    mf_float *p, 
    mf_float *q, 
    mf_float *pG, 
    mf_float *qG,
    mf_int d_begin,
    mf_int d_end,
    __m128 &XMMeta,
    __m128 &XMMlambda_p1,
    __m128 &XMMlambda_q1,
    __m128 &XMMlambda_p2,
    __m128 &XMMlambda_q2,
    __m128 &XMMe,
    __m128 &XMMrk,
    bool do_nmf)
{
    __m128 XMMpG = _mm_load1_ps(pG);
    __m128 XMMqG = _mm_load1_ps(qG);
    __m128 XMMeta_p = 
        _mm_mul_ps(XMMeta, _mm_rsqrt_ps(XMMpG));
    __m128 XMMeta_q = 
        _mm_mul_ps(XMMeta, _mm_rsqrt_ps(XMMqG));
    __m128 XMMpG1 = _mm_setzero_ps();
    __m128 XMMqG1 = _mm_setzero_ps();

    for(mf_int d = d_begin; d < d_end; d += 4)
    {
        __m128 XMMp = _mm_load_ps(p+d);
        __m128 XMMq = _mm_load_ps(q+d);

        __m128 XMMpg = _mm_sub_ps(_mm_mul_ps(XMMlambda_p2, XMMp),
                       _mm_mul_ps(XMMe, XMMq));
        __m128 XMMqg = _mm_sub_ps(_mm_mul_ps(XMMlambda_q2, XMMq),
                       _mm_mul_ps(XMMe, XMMp));

        XMMpG1 = _mm_add_ps(XMMpG1, _mm_mul_ps(XMMpg, XMMpg));
        XMMqG1 = _mm_add_ps(XMMqG1, _mm_mul_ps(XMMqg, XMMqg));

        XMMp = _mm_sub_ps(XMMp, _mm_mul_ps(XMMeta_p, XMMpg));
        XMMq = _mm_sub_ps(XMMq, _mm_mul_ps(XMMeta_q, XMMqg));

        mf_float tmp = 0;
        _mm_store_ss(&tmp, XMMlambda_p1);
        if(tmp > 0)
        {
            __m128 XMMflip = _mm_and_ps(_mm_cmple_ps(XMMp, _mm_set1_ps(0.0f)),
                               _mm_set1_ps(-0.0f));
            XMMp = _mm_xor_ps(XMMflip, _mm_max_ps(_mm_sub_ps(_mm_xor_ps(XMMp, XMMflip),
                   _mm_mul_ps(XMMeta_p, XMMlambda_p1)), _mm_set1_ps(0.0f)));
        }

        _mm_store_ss(&tmp, XMMlambda_q1);
        if(tmp > 0)
        {
            __m128 XMMflip = _mm_and_ps(_mm_cmple_ps(XMMq, _mm_set1_ps(0.0f)),
                               _mm_set1_ps(-0.0f));
            XMMq = _mm_xor_ps(XMMflip, _mm_max_ps(_mm_sub_ps(_mm_xor_ps(XMMq, XMMflip),
                   _mm_mul_ps(XMMeta_q, XMMlambda_q1)), _mm_set1_ps(0.0f)));
        }

        if(do_nmf)
        {
            XMMp = _mm_max_ps(XMMp, _mm_set1_ps(0.0f));
            XMMq = _mm_max_ps(XMMq, _mm_set1_ps(0.0f));
        }

        _mm_store_ps(p+d, XMMp);
        _mm_store_ps(q+d, XMMq);
    }

    XMMpG1 = _mm_hadd_ps(XMMpG1, XMMpG1);
    XMMpG1 = _mm_hadd_ps(XMMpG1, XMMpG1);
    XMMqG1 = _mm_hadd_ps(XMMqG1, XMMqG1);
    XMMqG1 = _mm_hadd_ps(XMMqG1, XMMqG1);

    XMMpG = _mm_add_ps(XMMpG, _mm_mul_ps(XMMpG1, XMMrk));
    XMMqG = _mm_add_ps(XMMqG, _mm_mul_ps(XMMqG1, XMMrk));

    _mm_store_ss(pG, XMMpG);
    _mm_store_ss(qG, XMMqG);
}
#elif defined USEAVX
inline void sg_update(
    mf_float *p, 
    mf_float *q, 
    mf_float *pG, 
    mf_float *qG,
    mf_int d_begin,
    mf_int d_end,
    __m256 &XMMeta,
    __m256 &XMMlambda_p1,
    __m256 &XMMlambda_q1,
    __m256 &XMMlambda_p2,
    __m256 &XMMlambda_q2,
    __m256 &XMMe,
    __m256 &XMMrk,
    bool do_nmf)
{
    __m256 XMMpG = _mm256_broadcast_ss(pG);
    __m256 XMMqG = _mm256_broadcast_ss(qG);
    __m256 XMMeta_p = 
        _mm256_mul_ps(XMMeta, _mm256_rsqrt_ps(XMMpG));
    __m256 XMMeta_q = 
        _mm256_mul_ps(XMMeta, _mm256_rsqrt_ps(XMMqG));
    __m256 XMMpG1 = _mm256_setzero_ps();
    __m256 XMMqG1 = _mm256_setzero_ps();

    for(mf_int d = d_begin; d < d_end; d += 8)
    {
        __m256 XMMp = _mm256_load_ps(p+d);
        __m256 XMMq = _mm256_load_ps(q+d);

        __m256 XMMpg = _mm256_sub_ps(_mm256_mul_ps(XMMlambda_p2, XMMp),
                                  _mm256_mul_ps(XMMe, XMMq));
        __m256 XMMqg = _mm256_sub_ps(_mm256_mul_ps(XMMlambda_q2, XMMq),
                                  _mm256_mul_ps(XMMe, XMMp));

        XMMpG1 = 
            _mm256_add_ps(XMMpG1, _mm256_mul_ps(XMMpg, XMMpg));
        XMMqG1 = 
            _mm256_add_ps(XMMqG1, _mm256_mul_ps(XMMqg, XMMqg));

        XMMp = _mm256_sub_ps(XMMp, _mm256_mul_ps(XMMeta_p, XMMpg));
        XMMq = _mm256_sub_ps(XMMq, _mm256_mul_ps(XMMeta_q, XMMqg));

        mf_float tmp = 0;
        _mm_store_ss(&tmp, _mm256_castps256_ps128(XMMlambda_p1));
        if(tmp > 0)
        {
            __m256 XMMflip = _mm256_and_ps(_mm256_cmp_ps(XMMp, _mm256_set1_ps(0.0f), 2),
                             _mm256_set1_ps(-0.0f));
            XMMp = _mm256_xor_ps(XMMflip, _mm256_max_ps(_mm256_sub_ps(_mm256_xor_ps(XMMp, XMMflip),
                   _mm256_mul_ps(XMMeta_p, XMMlambda_p1)), _mm256_set1_ps(0.0f)));
        }

        _mm_store_ss(&tmp, _mm256_castps256_ps128(XMMlambda_q1));
        if(tmp > 0)
        {
            __m256 XMMflip = _mm256_and_ps(_mm256_cmp_ps(XMMq, _mm256_set1_ps(0.0f), 2),
                             _mm256_set1_ps(-0.0f));
            XMMq = _mm256_xor_ps(XMMflip, _mm256_max_ps(_mm256_sub_ps(_mm256_xor_ps(XMMq, XMMflip),
                   _mm256_mul_ps(XMMeta_q, XMMlambda_q1)), _mm256_set1_ps(0.0f)));
        }

        if(do_nmf)
        {
            XMMp = _mm256_max_ps(XMMp, _mm256_set1_ps(0));
            XMMq = _mm256_max_ps(XMMq, _mm256_set1_ps(0));
        }

        _mm256_store_ps(p+d, XMMp);
        _mm256_store_ps(q+d, XMMq);
    }
    XMMpG1 = _mm256_add_ps(XMMpG1, _mm256_permute2f128_ps(XMMpG1, XMMpG1, 1));
    XMMpG1 = _mm256_hadd_ps(XMMpG1, XMMpG1);
    XMMpG1 = _mm256_hadd_ps(XMMpG1, XMMpG1);

    XMMqG1 = _mm256_add_ps(XMMqG1, _mm256_permute2f128_ps(XMMqG1, XMMqG1, 1));
    XMMqG1 = _mm256_hadd_ps(XMMqG1, XMMqG1);
    XMMqG1 = _mm256_hadd_ps(XMMqG1, XMMqG1);

    XMMpG = _mm256_add_ps(XMMpG, _mm256_mul_ps(XMMpG1, XMMrk));
    XMMqG = _mm256_add_ps(XMMqG, _mm256_mul_ps(XMMqG1, XMMrk));

    _mm_store_ss(pG, _mm256_castps256_ps128(XMMpG));
    _mm_store_ss(qG, _mm256_castps256_ps128(XMMqG));
}
#else

float qrsqrt(float x)
{
    float xhalf = 0.5f*x;
    uint32_t i;
    memcpy(&i, &x, sizeof(i));
    i = 0x5f375a86 - (i>>1);
    memcpy(&x, &i, sizeof(i));
    x = x*(1.5f - xhalf*x*x);
    return x;
}

inline void sg_update(
    mf_float *p, 
    mf_float *q, 
    mf_float *pG, 
    mf_float *qG,
    mf_int d_begin,
    mf_int d_end,
    mf_float eta,
    mf_float lambda_p1,
    mf_float lambda_q1,
    mf_float lambda_p2,
    mf_float lambda_q2,
    mf_float error,
    mf_float rk,
    bool do_nmf)
{
    mf_float eta_p = eta*qrsqrt(*pG);
    mf_float eta_q = eta*qrsqrt(*qG);

    mf_float pG1 = 0;
    mf_float qG1 = 0;

    for(mf_int d = d_begin; d < d_end; d++)
    {
        mf_float gp = -error*q[d]+lambda_p2*p[d];
        mf_float gq = -error*p[d]+lambda_q2*q[d];

        pG1 += gp*gp;
        qG1 += gq*gq;

        p[d] -= eta_p*gp;
        q[d] -= eta_q*gq;

        if(lambda_p1 > 0)
        {
            mf_float p1 = max(abs(p[d])-lambda_p1*eta_p, 0.0f);
            p[d] = p[d] >= 0? p1: -p1;
        }

        if(lambda_q1 > 0)
        {
            mf_float q1 = max(abs(q[d])-lambda_q1*eta_q, 0.0f);
            q[d] = q[d] >= 0? q1: -q1;
        }

        if(do_nmf)
        {
            p[d] = max(p[d], (mf_float)0.0f);
            q[d] = max(q[d], (mf_float)0.0f);
        }
    }

    *pG += pG1*rk;
    *qG += qG1*rk;
}
#endif

void sg(vector<mf_node*> &ptrs, mf_model &model, Scheduler &sched, 
        mf_parameter param, bool &slow_only, mf_float *PG, mf_float *QG)
{
    mf_float * P = model.P;
    mf_float * Q = model.Q;

#if defined USESSE
    __m128 XMMlambda_p1 = _mm_set1_ps(param.lambda_p1);
    __m128 XMMlambda_q1 = _mm_set1_ps(param.lambda_q1);
    __m128 XMMlambda_p2 = _mm_set1_ps(param.lambda_p2);
    __m128 XMMlambda_q2 = _mm_set1_ps(param.lambda_q2);
    __m128 XMMeta = _mm_set1_ps(param.eta);
    __m128 XMMrk_slow = _mm_set1_ps(1.0/kALIGN);
    __m128 XMMrk_fast = _mm_set1_ps(1.0/(model.k-kALIGN));
    while(true)
    {
        mf_int block = sched.get_job();
        __m128d XMMloss = _mm_setzero_pd();
        for(mf_node *N = ptrs[block]; N != ptrs[block+1]; N++)
        {
            mf_float *p = P+(mf_long)N->u*model.k;
            mf_float *q = Q+(mf_long)N->v*model.k;
            mf_float *pG = PG+N->u*2;
            mf_float *qG = QG+N->v*2;

            __m128 XMMz = _mm_setzero_ps();
            for(mf_int d = 0; d < model.k; d+= 4)
                XMMz = _mm_add_ps(XMMz, _mm_mul_ps(
                       _mm_load_ps(p+d), _mm_load_ps(q+d)));
            XMMz = _mm_hadd_ps(XMMz, XMMz);
            XMMz = _mm_hadd_ps(XMMz, XMMz);

            mf_float z = 0;
            switch(param.solver)
            {
                case 0:
                    XMMz = _mm_sub_ps(_mm_set1_ps(N->r), XMMz);
                    XMMloss = _mm_add_pd(XMMloss, _mm_cvtps_pd(
                              _mm_mul_ps(XMMz, XMMz)));
                    break;
                case 1:
                    _mm_store_ss(&z, XMMz);
                    if(N->r > 0)
                    {
                        z = exp(-z);
                        XMMloss = _mm_add_pd(XMMloss, _mm_set1_pd(log(1+z)));
                        XMMz = _mm_set1_ps(z/(1+z));
                    }
                    else
                    {
                        z = exp(z);
                        XMMloss = _mm_add_pd(XMMloss, _mm_set1_pd(log(1+z)));
                        XMMz = _mm_set1_ps(-z/(1+z));
                    }
                    break;
                default:
                    throw invalid_argument("unsupported loss function");
                    break;
            }

            sg_update(p, q, pG, qG, 0, kALIGN, XMMeta, XMMlambda_p1, XMMlambda_q1,
                      XMMlambda_p2, XMMlambda_q2, XMMz, XMMrk_slow, param.do_nmf);

            if(slow_only)
                continue;

            pG++;
            qG++;
            sg_update(p, q, pG, qG, kALIGN, model.k, XMMeta, XMMlambda_p1, XMMlambda_q1,
                      XMMlambda_p2, XMMlambda_q2, XMMz, XMMrk_fast, param.do_nmf);
        }
        mf_double loss;
        _mm_store_sd(&loss, XMMloss);
        sched.put_job(block, loss);
        if(sched.is_terminated())
            break;
    }
#elif defined USEAVX
    __m256 XMMlambda_p1 = _mm256_set1_ps(param.lambda_p1);
    __m256 XMMlambda_q1 = _mm256_set1_ps(param.lambda_q1);
    __m256 XMMlambda_p2 = _mm256_set1_ps(param.lambda_p2);
    __m256 XMMlambda_q2 = _mm256_set1_ps(param.lambda_q2);
    __m256 XMMeta = _mm256_set1_ps(param.eta);
    __m256 XMMrk_slow = _mm256_set1_ps(1.0/kALIGN);
    __m256 XMMrk_fast = _mm256_set1_ps(1.0/(model.k-kALIGN));
    while(true)
    {
        mf_int block = sched.get_job();
        __m128d XMMloss = _mm_setzero_pd();
        for(mf_node *N = ptrs[block]; N != ptrs[block+1]; N++)
        {
            mf_float *p = P+(mf_long)N->u*model.k;
            mf_float *q = Q+(mf_long)N->v*model.k;
            mf_float *pG = PG+N->u*2;
            mf_float *qG = QG+N->v*2;

            __m256 XMMz = _mm256_setzero_ps();
            for(mf_int d = 0; d < model.k; d+= 8)
                XMMz = _mm256_add_ps(XMMz, _mm256_mul_ps(
                       _mm256_load_ps(p+d), _mm256_load_ps(q+d)));
            XMMz = _mm256_hadd_ps(XMMz, XMMz);
            XMMz = _mm256_hadd_ps(XMMz, XMMz);
            XMMz = _mm256_hadd_ps(XMMz, XMMz);
            
            mf_float z = 0;
            switch(param.solver)
            {
                case 0:
                    XMMz = _mm256_sub_ps(_mm256_set1_ps(N->r-z), XMMz);
                    XMMloss = _mm_add_pd(XMMloss,
                              _mm_cvtps_pd(_mm256_castps256_ps128(
                              _mm256_mul_ps(XMMz, XMMz))));
                    break;
                case 1:
                    _mm_store1_ps(&z, _mm256_castps256_ps128(XMMz));
                    if(N->r > 0)
                    {
                        z = exp(-z);
                        XMMloss = _mm_add_pd(XMMloss, _mm_set1_pd(log(1.0+z)));
                        XMMz = _mm256_set1_ps(z/(1+z));
                    }
                    else
                    {
                        z = exp(z);
                        XMMloss = _mm_add_pd(XMMloss, _mm_set1_pd(log(1.0+z)));
                        XMMz = _mm256_set1_ps(-z/(1+z));
                    }
                    break;
                default:
                    throw invalid_argument("unsupported loss function");
                    break;
            }

            sg_update(p, q, pG, qG, 0, kALIGN, XMMeta, XMMlambda_p1, XMMlambda_q1,
                      XMMlambda_p2, XMMlambda_q2, XMMz, XMMrk_slow, param.do_nmf);

            if(slow_only)
                continue;

            pG++;
            qG++;
            sg_update(p, q, pG, qG, kALIGN, model.k, XMMeta, XMMlambda_p1, XMMlambda_q1,
                      XMMlambda_p2, XMMlambda_q2, XMMz, XMMrk_fast, param.do_nmf);
        }
        mf_double loss;
        _mm_store_sd(&loss, XMMloss);
        sched.put_job(block, loss);
        if(sched.is_terminated())
            break;
    }
#else
    mf_float rk_slow = 1.0/kALIGN;
    mf_float rk_fast = 1.0/(model.k-kALIGN);
    while(true)
    {
        mf_int block = sched.get_job();
        mf_double loss = 0;
        for(mf_node *N = ptrs[block]; N != ptrs[block+1]; N++)
        {
            mf_float *p = P+(mf_long)N->u*model.k;
            mf_float *q = Q+(mf_long)N->v*model.k;
            mf_float *pG = PG+N->u*2;
            mf_float *qG = QG+N->v*2;

            mf_float z = 0;
            for(mf_int d = 0; d < model.k; d++)
                z += p[d]*q[d];

            switch(param.solver)
            {
                case 0:
                    z = N->r-z;
                    loss += z*z;
                    break;
                case 1:
                    if(N->r > 0)
                    {
                        z = exp(-z);
                        loss += log(1+z);
                        z = z/(1+z);
                    }
                    else
                    {
                        z = exp(z);
                        loss += log(1+z);
                        z = -z/(1+z);
                    }
                    break;
                 default:
                    throw invalid_argument("unsupported loss function");
                    break;
            }

            sg_update(p, q, pG, qG, 0, kALIGN, param.eta, 
                      param.lambda_p1, param.lambda_q1,
                      param.lambda_p2, param.lambda_q2,
                      z, rk_slow, param.do_nmf);

            if(slow_only)
                continue;

            pG++;
            qG++;

            sg_update(p, q, pG, qG, kALIGN, model.k, param.eta, 
                      param.lambda_p1, param.lambda_q1,
                      param.lambda_p2, param.lambda_q2,
                      z, rk_fast, param.do_nmf);

        }
        sched.put_job(block, loss);
        if(sched.is_terminated())
            break;
    }
#endif
}

class Utility
{
public:
    Utility (mf_int s, mf_int n);
    void shuffle_problem(mf_problem &prob, vector<mf_int> &p_map, vector<mf_int> &q_map);
    vector<mf_node*> grid_problem(mf_problem &prob, mf_int nr_bins);
    mf_float calc_std_dev(mf_problem &prob);
    void scale_problem(mf_problem &prob, mf_float scale);
    mf_double calc_reg1(mf_model &model, mf_float lambda_p, mf_float lambda_q,
            vector<mf_int> &omega_p, vector<mf_int> &omega_q);
    mf_double calc_reg2(mf_model &model, mf_float lambda_p, mf_float lambda_q,
            vector<mf_int> &omega_p, vector<mf_int> &omega_q);
    mf_double calc_loss(mf_node *R, mf_long size, mf_model const &model);
    string get_criterion_legend();
    mf_double get_criterion(mf_double const loss, mf_long const size);
    mf_double get_criterion(mf_problem const &prob, mf_model const &model);
    void scale_model(mf_model &model, mf_float scale);

    static mf_problem* copy_problem(mf_problem const *prob, bool copy_data);
    static vector<mf_int> gen_random_map(mf_int size);
    static mf_float* malloc_aligned_float(mf_long size);
    static mf_model* init_model(shared_ptr<mf_problem> prob, mf_int k_real, mf_int k_aligned);
    static mf_float inner_product(mf_float *p, mf_float *q, mf_int k);
    static vector<mf_int> gen_inv_map(vector<mf_int> &map);
    static void shrink_model(mf_model &model, mf_int k_new);
    static void shuffle_model(mf_model &model, vector<mf_int> &p_map, vector<mf_int> &q_map);

private:
    mf_int solver;
    mf_int nr_threads;
};

Utility::Utility(mf_int s, mf_int n):
    solver(s), nr_threads(n)
{ }

mf_float Utility::calc_std_dev(mf_problem &prob)
{
    mf_double avg = 0;
#if defined USEOMP
#pragma omp parallel for num_threads(nr_threads) schedule(static) reduction(+: avg)
#endif
    for(mf_long i = 0; i < prob.nnz; i++)
        avg += prob.R[i].r;
    avg /= prob.nnz;

    mf_double std_dev = 0;
#if defined USEOMP
#pragma omp parallel for num_threads(nr_threads) schedule(static) reduction(+: std_dev)
#endif
    for(mf_long i = 0; i < prob.nnz; i++)
        std_dev += (prob.R[i].r-avg)*(prob.R[i].r-avg);
    std_dev = sqrt(std_dev/prob.nnz);

    return (mf_float)std_dev;
}

void Utility::scale_problem(mf_problem &prob, mf_float scale)
{
    if(scale == 1.0)
        return;

#if defined USEOMP
#pragma omp parallel for num_threads(nr_threads) schedule(static)
#endif
    for(mf_long i = 0; i < prob.nnz; i++)
        prob.R[i].r *= scale;
}

void Utility::scale_model(mf_model &model, mf_float scale)
{
    if(scale == 1.0)
        return;

    mf_int k = model.k;

    auto scale1 = [&] (mf_float *ptr, mf_int size)
    {
#if defined USEOMP
#pragma omp parallel for num_threads(nr_threads) schedule(static)
#endif
        for(mf_int i = 0; i < size; i++)
        {
            mf_float *ptr1 = ptr+(mf_long)i*model.k;
            for(mf_int d = 0; d < k; d++)
                ptr1[d] *= scale;
        }
    };

    scale1(model.P, model.m);
    scale1(model.Q, model.n);
}

mf_float Utility::inner_product(mf_float *p, mf_float *q, mf_int k)
{
#if defined USESSE
    __m128 XMM = _mm_setzero_ps();
    for(mf_int d = 0; d < k; d += 4)
        XMM = _mm_add_ps(XMM, _mm_mul_ps(
                  _mm_load_ps(p+d), _mm_load_ps(q+d)));
    XMM = _mm_hadd_ps(XMM, XMM);
    XMM = _mm_hadd_ps(XMM, XMM);
    mf_float product;
    _mm_store_ss(&product, XMM);
    return product;
#elif defined USEAVX
    __m256 XMM = _mm256_setzero_ps();
    for(mf_int d = 0; d < k; d += 8)
        XMM = _mm256_add_ps(XMM, _mm256_mul_ps(
                  _mm256_load_ps(p+d), _mm256_load_ps(q+d)));
    XMM = _mm256_add_ps(XMM, _mm256_permute2f128_ps(XMM, XMM, 1));
    XMM = _mm256_hadd_ps(XMM, XMM);
    XMM = _mm256_hadd_ps(XMM, XMM);
    mf_float product;
    _mm_store_ss(&product, _mm256_castps256_ps128(XMM));
    return product;
#else
    return std::inner_product(p, p+k, q, (mf_float)0.0);
#endif
}

mf_double Utility::calc_reg1(mf_model &model, mf_float lambda_p, mf_float lambda_q,
        vector<mf_int> &omega_p, vector<mf_int> &omega_q)
{
    auto calc_reg1_core = [&] (mf_float *ptr, mf_int size, vector<mf_int> &omega)
    {
        mf_double reg = 0;
        for(mf_int i = 0; i < size; i++)
        {
            mf_float tmp = 0; 
            for(mf_int j = 0; j < model.k; j++)
                tmp += abs(ptr[i*model.k+j]);
            reg += omega[i]*tmp;
        }
        return reg;
    };

    return lambda_p*calc_reg1_core(model.P, model.m, omega_p)+
           lambda_q*calc_reg1_core(model.Q, model.n, omega_q);
}

mf_double Utility::calc_reg2(mf_model &model, mf_float lambda_p, mf_float lambda_q,
        vector<mf_int> &omega_p, vector<mf_int> &omega_q)
{
    auto calc_reg2_core = [&] (mf_float *ptr, mf_int size, vector<mf_int> &omega)
    {
        mf_double reg = 0;
#if defined USEOMP
#pragma omp parallel for num_threads(nr_threads) schedule(static) reduction(+:reg)
#endif
        for(mf_int i = 0; i < size; i++)
        {
            mf_float *ptr1 = ptr+(mf_long)i*model.k;
            reg += omega[i]*Utility::inner_product(ptr1, ptr1, model.k);
        }

        return reg;
    };

    return lambda_p*calc_reg2_core(model.P, model.m, omega_p) + 
           lambda_q*calc_reg2_core(model.Q, model.n, omega_q);
}

mf_double Utility::calc_loss(mf_node *R, mf_long size, mf_model const &model)
{
    mf_double loss = 0;
#if defined USEOMP
#pragma omp parallel for num_threads(nr_threads) schedule(static) reduction(+:loss)
#endif
    for(mf_long i = 0; i < size; i++)
    {
        mf_node &N = R[i];
        mf_float z = mf_predict(&model, N.u, N.v);
        switch(solver)
        {
            case 0:
                loss += pow(N.r-z, 2);
                break;
            case 1:
                if(N.r > 0)
                    loss += log(1.0+exp(-z));
                else
                    loss += log(1.0+exp(z));
                    break;
             default:
                throw invalid_argument("unsupported loss function");
                break;
        }
    }
    return loss;
}

string Utility::get_criterion_legend()
{
    switch(solver)
    {
        case 0:
            return string("rmse");
            break;
        case 1:
            return string("logloss");
            break;
        default:
            return string();
            break;
     }
}

mf_double Utility::get_criterion(mf_double const loss, mf_long const size)
{
    if(size == 0)
        return 0;

    switch(solver)
    {
        case 0:
            return sqrt(loss/size);
            break;
        case 1:
            return loss/size;
            break;
        default:
            throw invalid_argument("unsupported loss function");
            break;
    }
}

mf_double Utility::get_criterion(mf_problem const &prob, mf_model const &model)
{
    if(prob.nnz == 0)
        return 0;

    mf_double loss = calc_loss(prob.R, prob.nnz, model);

    return get_criterion(loss, prob.nnz);
}

void Utility::shuffle_problem(
    mf_problem &prob, 
    vector<mf_int> &p_map, 
    vector<mf_int> &q_map)
{
#if defined USEOMP
#pragma omp parallel for num_threads(nr_threads) schedule(static)
#endif
    for(mf_long i = 0; i < prob.nnz; i++)
    {
        mf_node &N = prob.R[i];
        if(N.u < p_map.size())
            N.u = p_map[N.u];
        if(N.v < q_map.size())
            N.v = q_map[N.v];
    }
}

vector<mf_node*> Utility::grid_problem(mf_problem &prob, mf_int nr_bins)
{
    vector<mf_long> counts(nr_bins*nr_bins, 0);

    mf_int seg_p = (mf_int)ceil((double)prob.m/nr_bins);
    mf_int seg_q = (mf_int)ceil((double)prob.n/nr_bins);

    auto get_block = [=] (mf_int u, mf_int v)
    {
        return (u/seg_p)*nr_bins+v/seg_q;
    };

    for(mf_long i = 0; i < prob.nnz; i++)
    {
        mf_node &N = prob.R[i];
        mf_int block = get_block(N.u, N.v);
        counts[block]++;
    }

    vector<mf_node*> ptrs(nr_bins*nr_bins+1);
    mf_node *ptr = prob.R;
    ptrs[0] = ptr;
    for(mf_int block = 0; block < nr_bins*nr_bins; block++)
        ptrs[block+1] = ptrs[block] + counts[block];

    vector<mf_node*> pivots(ptrs.begin(), ptrs.end()-1);
    for(mf_int block = 0; block < nr_bins*nr_bins; block++)
    {
        for(mf_node* pivot = pivots[block]; pivot != ptrs[block+1];)
        {
            mf_int curr_block = get_block(pivot->u, pivot->v);
            if(curr_block == block)
            {
                pivot++;
                continue;
            }

            mf_node *next = pivots[curr_block];
            swap(*pivot, *next);
            pivots[curr_block]++;
        }
    }

    struct sort_node_by_p
    {
        bool operator() (mf_node const &lhs, mf_node const &rhs)
        {
            return tie(lhs.u, lhs.v) < tie(rhs.u, rhs.v);
        }
    };

    struct sort_node_by_q
    {
        bool operator() (mf_node const &lhs, mf_node const &rhs)
        {
            return tie(lhs.v, lhs.u) < tie(rhs.v, rhs.u);
        }
    };

#if defined USEOMP
#pragma omp parallel for num_threads(nr_threads) schedule(dynamic)
#endif
    for(mf_int block = 0; block < nr_bins*nr_bins; block++)
    {
        if(prob.m > prob.n)
            sort(ptrs[block], ptrs[block+1], sort_node_by_p());
        else
            sort(ptrs[block], ptrs[block+1], sort_node_by_q());
    }

    return ptrs;
}

mf_float* Utility::malloc_aligned_float(mf_long size)
{
    void *ptr;
#ifdef _WIN32
    ptr = _aligned_malloc(size*sizeof(mf_float), kALIGNByte);
    if(ptr == nullptr)
        throw bad_alloc();
#else
    int status = posix_memalign(&ptr, kALIGNByte, size*sizeof(mf_float));
    if(status != 0)
        throw bad_alloc();
#endif
 
    return (mf_float*)ptr;
}

mf_model* Utility::init_model(shared_ptr<mf_problem> prob, mf_int k_real, mf_int k_aligned)
{
    mf_model *model = new mf_model;
    model->m = prob->m;
    model->n = prob->n;
    model->k = k_aligned;
    model->P = nullptr;
    model->Q = nullptr;

    mf_float scale = sqrt(1.0/k_real);
    default_random_engine generator;
    uniform_real_distribution<mf_float> distribution(0.0, 1.0);

    try
    {
        model->P = Utility::malloc_aligned_float((mf_long)model->m*model->k);
        model->Q = Utility::malloc_aligned_float((mf_long)model->n*model->k);
    }
    catch(bad_alloc const &e)
    {
        mf_destroy_model(&model);
        throw; 
    }

    set<mf_int> u_set;
    set<mf_int> v_set;

    for (mf_int i=0; i<prob->nnz; i++)
    {
        u_set.insert(prob->R[i].u);
        v_set.insert(prob->R[i].v);
    }

    auto init1 = [&](mf_float *start_ptr, mf_int count, set<mf_int> nz_set)
    {
        memset(start_ptr, 0, sizeof(mf_float)*count*model->k);
        for (auto it = nz_set.begin(); it != nz_set.end(); it++)
        {
            mf_float * ptr = start_ptr + (mf_long)(*it)*model->k;
            for(mf_long d = 0; d < k_real; d++, ptr++)
                *ptr = (mf_float)(distribution(generator)*scale);
        }
    };

    init1(model->P, prob->m, u_set);
    init1(model->Q, prob->n, v_set);

    return model;
}

vector<mf_int> Utility::gen_random_map(mf_int size)
{
    srand(0);
    vector<mf_int> map(size, 0);
    for(mf_int i = 0; i < size; i++)
        map[i] = i;
    random_shuffle(map.begin(), map.end());
    return map;
}

vector<mf_int> Utility::gen_inv_map(vector<mf_int> &map)
{
    vector<mf_int> inv_map(map.size());
    for(mf_int i = 0; i < map.size(); i++)
      inv_map[map[i]] = i;
    return inv_map;
}

void Utility::shuffle_model(
    mf_model &model, 
    vector<mf_int> &p_map, 
    vector<mf_int> &q_map)
{
    auto inv_shuffle1 = [] (mf_float *vec, vector<mf_int> &map,
                            mf_int size, mf_int k)
    {
        for(mf_int pivot = 0; pivot < size;)
        {
            if(pivot == map[pivot])
            {
                ++pivot;
                continue;
            }

            mf_int next = map[pivot];

            for(mf_int d = 0; d < k; d++)
                swap(*(vec+(mf_long)pivot*k+d), *(vec+(mf_long)next*k+d));

            map[pivot] = map[next];
            map[next] = next;
        }
    };

    inv_shuffle1(model.P, p_map, model.m, model.k);
    inv_shuffle1(model.Q, q_map, model.n, model.k);
}

void Utility::shrink_model(mf_model &model, mf_int k_new)
{
    mf_int k_old = model.k;
    model.k = k_new;

    auto shrink1 = [&] (mf_float *ptr, mf_int size)
    {
        for(mf_int i = 0; i < size; i++)
        {
            mf_float *src = ptr+(mf_long)i*k_old;
            mf_float *dst = ptr+(mf_long)i*k_new;
            copy(src, src+k_new, dst);
        }
    };

    shrink1(model.P, model.m);
    shrink1(model.Q, model.n);
}

mf_problem* Utility::copy_problem(mf_problem const *prob, bool copy_data)
{
    mf_problem *new_prob = new mf_problem;

    if(prob == nullptr)
    {
        new_prob->m = 0;
        new_prob->n = 0;
        new_prob->nnz = 0;
        new_prob->R = nullptr;

        return new_prob;
    }

    new_prob->m = prob->m;
    new_prob->n = prob->n;
    new_prob->nnz = prob->nnz;

    if(copy_data)
    {
        try
        {
            new_prob->R = new mf_node[prob->nnz];
            copy(prob->R, prob->R+prob->nnz, new_prob->R);
        }
        catch(...)
        {
            delete new_prob;
            throw;
        }
    }
    else
    {
        new_prob->R = prob->R; 
    }

    return new_prob;
}

shared_ptr<mf_model> fpsg(
    mf_problem const *tr_, 
    mf_problem const *va_, 
    mf_parameter param, 
    vector<mf_int> cv_blocks=vector<mf_int>(),
    mf_double *cv_loss=nullptr, 
    mf_long *cv_count=nullptr)
{
    param.nr_bins = max(param.nr_bins, 2*param.nr_threads);
    Utility util(param.solver, param.nr_threads);

    shared_ptr<mf_problem> tr, va;
    if(param.copy_data)
    {
        struct deleter
        {
            void operator() (mf_problem *prob)
            {
                delete[] prob->R;
                delete prob;
            }
        };

        tr = shared_ptr<mf_problem>(Utility::copy_problem(tr_, true), deleter());
        va = shared_ptr<mf_problem>(Utility::copy_problem(va_, true), deleter());
    }
    else
    {
        tr = shared_ptr<mf_problem>(Utility::copy_problem(tr_, false));
        va = shared_ptr<mf_problem>(Utility::copy_problem(va_, false));
    }

    vector<mf_int> p_map = Utility::gen_random_map(tr->m);
    vector<mf_int> q_map = Utility::gen_random_map(tr->n);

    util.shuffle_problem(*tr, p_map, q_map);
    util.shuffle_problem(*va, p_map, q_map);

    vector<mf_node*> ptrs = util.grid_problem(*tr, param.nr_bins);

    mf_int k_aligned = (mf_int)ceil(mf_double(param.k)/kALIGN)*kALIGN;

    shared_ptr<mf_model> model(Utility::init_model(tr, param.k, k_aligned), 
                               [] (mf_model *ptr) { mf_destroy_model(&ptr); });

    mf_float std_dev = param.solver != 0? 1: max((mf_float)1e-4, util.calc_std_dev(*tr));

    util.scale_problem(*tr, 1.0/std_dev);
    util.scale_problem(*va, 1.0/std_dev);
    param.lambda_p2 /= std_dev;
    param.lambda_q2 /= std_dev;
    param.lambda_p1 /= pow(std_dev, 1.5);
    param.lambda_q1 /= pow(std_dev, 1.5);

    Scheduler sched(param.nr_bins, param.nr_threads, cv_blocks);

    vector<mf_int> omega_p(tr->m, 0), omega_q(tr->n, 0);
    for(mf_long i = 0; i < tr->nnz; i++)
    {
        mf_node &N = tr->R[i];
        omega_p[N.u]++;
        omega_q[N.v]++;
    }

    bool slow_only = param.lambda_p1 == 0 && param.lambda_q1 == 0? true: false;

    vector<mf_float> PG(model->m*2, 1), QG(model->n*2, 1);

#if defined USESSE || defined USEAVX
    auto flush_zero_mode = _MM_GET_FLUSH_ZERO_MODE();
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#endif
    
    vector<thread> threads;
    for(mf_int i = 0; i < param.nr_threads; i++)
        threads.emplace_back(sg, ref(ptrs), ref(*model), ref(sched), param, 
                             ref(slow_only), PG.data(), QG.data());

    if(!param.quiet)
    {
        cout.width(4);
        cout << "iter";
        cout.width(13);
        cout << "tr_"+util.get_criterion_legend();
        if(va->nnz != 0)
        {
            cout.width(13);
            cout << "va_"+util.get_criterion_legend();
        }
        cout.width(13);
        cout << "obj";
        cout << "\n";
    }

    for(mf_int iter = 0; iter < param.nr_iters; iter++)
    {
        sched.wait_for_jobs_done();

        if(!param.quiet)
        {
            mf_double reg = util.calc_reg1(*model, param.lambda_p1, param.lambda_q1,
                            omega_p, omega_q)*std_dev*std_dev+
                            util.calc_reg2(*model, param.lambda_p2, param.lambda_q2,
                            omega_p, omega_q)*std_dev*std_dev;

            mf_double tr_loss = sched.get_loss()*std_dev*std_dev;

            mf_double tr_criterion = util.get_criterion(tr_loss, tr->nnz);
            
            cout.width(4);
            cout << iter;
            cout.width(13);
            cout << fixed << setprecision(4) << tr_criterion;
            if(va->nnz != 0)
            {
                mf_double va_criterion = util.get_criterion(*va, *model)*std_dev;
                cout.width(13);
                cout << fixed << setprecision(4) << va_criterion;
            }
            cout.width(13);
            cout << fixed << setprecision(4) << scientific << reg+tr_loss;
            cout << "\n" << flush;
        }

        if(iter == 0) 
            slow_only = false;

        sched.resume();
    }
    sched.terminate();
    
    for(auto &thread : threads)
        thread.join();

#if defined USESSE || defined USEAVX
    _MM_SET_FLUSH_ZERO_MODE(flush_zero_mode);
#endif

    mf_double loss = util.calc_loss(tr->R, tr->nnz, *model)*std_dev*std_dev;

    if(cv_loss != nullptr && cv_count != nullptr)
    {
        *cv_loss = 0;
        *cv_count = 0;
        for(auto block : cv_blocks)
        {
            *cv_loss += util.calc_loss(ptrs[block], ptrs[block+1]-ptrs[block], *model);
            *cv_count += ptrs[block+1]-ptrs[block];
        }
        *cv_loss *= std_dev*std_dev;
    }

    vector<mf_int> inv_p_map = Utility::gen_inv_map(p_map);
    vector<mf_int> inv_q_map = Utility::gen_inv_map(q_map);

    if(!param.copy_data)
    {
        util.scale_problem(*tr, std_dev);
        util.scale_problem(*va, std_dev);
        util.shuffle_problem(*tr, inv_p_map, inv_q_map);
        util.shuffle_problem(*va, inv_p_map, inv_q_map);
    }

    util.scale_model(*model, sqrt(std_dev));
    Utility::shrink_model(*model, param.k);
    Utility::shuffle_model(*model, inv_p_map, inv_q_map);

    return model;
}

} // unnamed namespace

mf_model* mf_train_with_validation(
    mf_problem const *tr, 
    mf_problem const *va, 
    mf_parameter param)
{
    shared_ptr<mf_model> model = fpsg(tr, va, param);

    mf_model *model_ret = new mf_model;

    model_ret->m = model->m;
    model_ret->n = model->n;
    model_ret->k = model->k;

    model_ret->P = model->P;
    model->P = nullptr;

    model_ret->Q = model->Q;
    model->Q = nullptr;

    return model_ret;
}

mf_model* mf_train(mf_problem const *prob, mf_parameter param)
{
    return mf_train_with_validation(prob, nullptr, param);
}

mf_float mf_cross_validation(
    mf_problem const *prob, 
    mf_int nr_folds, 
    mf_parameter param)
{
    bool quiet = param.quiet;
    param.quiet = true;

    mf_int nr_bins = param.nr_bins;
    mf_int nr_blocks_per_fold = nr_bins*nr_bins/nr_folds;

    Utility util(param.solver, param.nr_threads);

    srand(0);
    vector<mf_int> cv_blocks;
    for(mf_int block = 0; block < nr_bins*nr_bins; block++)
        cv_blocks.push_back(block);
    random_shuffle(cv_blocks.begin(), cv_blocks.end());

    if(!quiet)
    {
        cout.width(4);
        cout << "fold";
        cout.width(10);
        cout << util.get_criterion_legend();
        cout << endl;
    }

    mf_double loss = 0;
    mf_int idx = 0;
    mf_long count = 0;
    for(mf_int fold = 0; fold < nr_folds; fold++)
    {
        mf_int begin = fold*nr_blocks_per_fold;
        mf_int end = min((fold+1)*nr_blocks_per_fold, nr_bins*nr_bins);

        vector<mf_int> cv_blocks1(cv_blocks.begin()+begin, 
                                  cv_blocks.begin()+end);

        mf_double loss1 = 0;
        mf_long count1 = 0;

        fpsg(prob, nullptr, param, cv_blocks1, &loss1, &count1);

        mf_float rmse1 = util.get_criterion(loss1, count1);

        if(!quiet)
        {
            cout.width(4);
            cout << fold;
            cout.width(10);
            cout << fixed << setprecision(4) << rmse1;
            cout << endl;
        }

        loss += loss1;
        count += count1;
    }

    mf_float rmse = util.get_criterion(loss, count);

    if(!quiet)
    {
        cout.width(14);
        cout.fill('=');
        cout << "" << endl;
        cout.fill(' ');
        cout.width(4);
        cout << "avg";
        cout.width(10);
        cout << fixed << setprecision(4) << rmse;
        cout << endl;
    }
    
    return rmse;
}

mf_int mf_save_model(mf_model const *model, char const *path)
{
    ofstream f(path);
    if(!f.is_open())
        return 1;

    f << "m " << model->m << endl;
    f << "n " << model->n << endl;
    f << "k " << model->k << endl;

    auto write = [&] (mf_float *ptr, mf_int size, char prefix)
    {
        for(mf_int i = 0; i < size; i++)
        {
            mf_float *ptr1 = ptr + (mf_long)i*model->k;
            f << prefix << i << " ";
            for(mf_int d = 0; d < model->k; d++)
                f << ptr1[d] << " ";
            f << endl;
        }
    };

    write(model->P, model->m, 'p');
    write(model->Q, model->n, 'q');

    return 0;
}

mf_model* mf_load_model(char const *path)
{
    ifstream f(path);
    if(!f.is_open())
        return nullptr;

    string dummy;

    mf_model *model = new mf_model;
    model->P = nullptr;
    model->Q = nullptr;

    f >> dummy >> model->m >> dummy >> model->n >> dummy >> model->k;

    try
    {
        model->P = Utility::malloc_aligned_float((mf_long)model->m*model->k);
        model->Q = Utility::malloc_aligned_float((mf_long)model->n*model->k);
    }
    catch(bad_alloc const &e)
    {
        mf_destroy_model(&model);
        return nullptr;
    }

    auto read = [&] (mf_float *ptr, mf_int size)
    {
        for(mf_int i = 0; i < size; i++)
        {
            mf_float *ptr1 = ptr + (mf_long)i*model->k;
            f >> dummy;
            for(mf_int d = 0; d < model->k; d++)
                f >> ptr1[d];
        }
    };

    read(model->P, model->m);
    read(model->Q, model->n);

    return model;
}

void mf_destroy_model(mf_model **model)
{
    if(model == nullptr || *model == nullptr)
        return;
#ifdef _WIN32
    _aligned_free((*model)->P);
    _aligned_free((*model)->Q);
#else
    free((*model)->P);
    free((*model)->Q);
#endif
    delete *model;
    *model = nullptr;
}

mf_float mf_predict(mf_model const *model, mf_int u, mf_int v)
{
    if(u < 0 || u >= model->m || v < 0 || v >= model->n)
        return 0.0f;

    mf_float *p = model->P+(mf_long)u*model->k;
    mf_float *q = model->Q+(mf_long)v*model->k;

    return std::inner_product(p, p+model->k, q, (mf_float)0);
}

mf_parameter mf_get_default_param()
{
    mf_parameter param;

    param.k = 8;
    param.nr_threads = 12;
    param.nr_bins = 20;
    param.nr_iters = 20;
    param.lambda_p1 = 0.0f;
    param.lambda_q1 = 0.0f;
    param.lambda_p2 = 0.1f;
    param.lambda_q2 = 0.1f;
    param.eta = 0.1f;
    param.do_nmf = false;
    param.quiet = false;
    param.copy_data = true;
    param.solver = 0;

    return param;
}

} // namespace mf
