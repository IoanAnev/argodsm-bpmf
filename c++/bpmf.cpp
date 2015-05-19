
#include <stdlib.h>     /* srand, rand */

#include <chrono>
#include <iostream>
#include <fstream>
#include <string>
#include <algorithm>

#include <unsupported/Eigen/SparseExtra>

#include "bpmf.h"

using namespace std;
using namespace Eigen;

typedef SparseMatrix<double> SparseMatrixD;

const int num_feat = 32;

const int alpha = 2;
const int nsims = 20;
const int burnin = 5;

double mean_rating = .0;

SparseMatrixD M, P;

typedef Matrix<double, num_feat, 1> VectorNd;
typedef Matrix<double, num_feat, num_feat> MatrixNNd;
typedef Matrix<double, num_feat, Dynamic> MatrixNXd;

VectorNd mu_u;
VectorNd mu_m;
MatrixNNd Lambda_u;
MatrixNNd Lambda_m;
MatrixNXd sample_u;
MatrixNXd sample_m;

// parameters of Inv-Whishart distribution (see paper for details)
MatrixNNd WI_u;
const int b0_u = 2;
const int df_u = num_feat;
VectorNd mu0_u;

MatrixNNd WI_m;
const int b0_m = 2;
const int df_m = num_feat;
VectorNd mu0_m;

void init() {
    mean_rating = M.sum() / M.nonZeros();
    Lambda_u.setIdentity();
    Lambda_m.setIdentity();

    sample_u = MatrixNXd(num_feat,M.rows());
    sample_m = MatrixNXd(num_feat,M.cols());
    sample_u.setZero();
    sample_m.setZero();

    // parameters of Inv-Whishart distribution (see paper for details)
    WI_u.setIdentity();
    mu0_u.setZero();

    WI_m.setIdentity();
    mu0_m.setZero();
}

pair<double,double> eval_probe_vec(const MatrixNXd &sample_m, const MatrixNXd &sample_u, double mean_rating)
{
    unsigned n = P.nonZeros();
    unsigned correct = 0;
    double diff = .0;
    for (int k=0; k<P.outerSize(); ++k)
        for (SparseMatrix<double>::InnerIterator it(P,k); it; ++it) {
            double prediction = sample_m.col(it.col()).dot(sample_u.col(it.row())) + mean_rating;
            //cout << "prediction: " << prediction - mean_rating << " + " << mean_rating << " = " << prediction << endl;
            //cout << "actual: " << it.value() << endl;
            correct += (it.value() < log10(200)) == (prediction < log10(200));
            diff += abs(it.value() - prediction);
        }
   
    return std::make_pair((double)correct / n, diff / n);
}

void sample_movie(MatrixNXd &s, int mm, const SparseMatrixD &mat, double mean_rating, 
    const MatrixNXd &samples, int alpha, const VectorNd &mu_u, const MatrixNNd &Lambda_u)
{
    int i = 0;
    MatrixNXd E(num_feat,mat.col(mm).nonZeros());
    VectorXd rr(mat.col(mm).nonZeros());
    //cout << "movie " << endl;
    for (SparseMatrixD::InnerIterator it(mat,mm); it; ++it, ++i) {
        // cout << "M[" << it.row() << "," << it.col() << "] = " << it.value() << endl;
        E.col(i) = samples.col(it.row());
        rr(i) = it.value() - mean_rating;
    }


    auto MM = E * E.transpose();
    auto MMs = alpha * MM;
    MatrixNNd covar = (Lambda_u + MMs).inverse();
    VectorNd MMrr = (E * rr) * alpha;
    auto U = Lambda_u * mu_u;
    auto mu = covar * (MMrr + U);

    MatrixNNd chol = covar.llt().matrixL();
#ifdef TEST_SAMPLE
    auto r(num_feat); r.setConstant(0.25);
#else
    auto r = nrandn(num_feat);
#endif
    s.col(mm) = chol * r + mu;

#ifdef TEST_SAMPLE
      cout << "movie " << mm << ":" << result.cols() << " x" << result.rows() << endl;
      cout << "mean rating " << mean_rating << endl;
      cout << "E = [" << E << "]" << endl;
      cout << "rr = [" << rr << "]" << endl;
      cout << "MM = [" << MM << "]" << endl;
      cout << "Lambda_u = [" << Lambda_u << "]" << endl;
      cout << "covar = [" << covar << "]" << endl;
      cout << "mu = [" << mu << "]" << endl;
      cout << "chol = [" << chol << "]" << endl;
      cout << "rand = [" << r <<"]" <<  endl;
      cout << "result = [" << result << "]" << endl;
#endif

}

#ifdef TEST_SAMPLE
void test() {
    MatrixNXd sample_u(M.rows());
    MatrixNXd sample_m(M.cols());

    mu_m.setZero();
    Lambda_m.setIdentity();
    sample_u.setConstant(2.0);
    Lambda_m *= 0.5;
    sample_m.col(0) = sample_movie(0, M, mean_rating, sample_u, alpha, mu_m, Lambda_m);
}

#else

void run() {
    auto start = std::chrono::steady_clock::now();

    SparseMatrixD Mt = M.transpose();

    std::cout << "Sampling" << endl;
    for(int i=0; i<nsims; ++i) {

      // Sample from movie hyperparams
      tie(mu_m, Lambda_m) = CondNormalWishart(sample_m, mu0_m, b0_m, WI_m, df_m);

      // Sample from user hyperparams
      tie(mu_u, Lambda_u) = CondNormalWishart(sample_u, mu0_u, b0_u, WI_u, df_u);

#pragma omp parallel for
      for(int mm = 0; mm < M.cols(); ++mm) {
        sample_movie(sample_m, mm, M, mean_rating, sample_u, alpha, mu_m, Lambda_m);
      }

#pragma omp parallel for
      for(int uu = 0; uu < M.rows(); ++uu) {
        sample_movie(sample_u, uu, Mt, mean_rating, sample_m, alpha, mu_u, Lambda_u);
      }

      auto eval = eval_probe_vec(sample_m, sample_u, mean_rating);
      double norm_u = sample_u.norm();
      double norm_m = sample_m.norm();
      auto end = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration<double>(end - start);
      double samples_per_sec = (i + 1) * (M.rows() + M.cols()) / elapsed.count();

      printf("Iteration %d:\t num_correct: %3.2f%%\tavg_diff: %3.2f\tFU(%6.2f)\tFM(%6.2f)\tSamples/sec: %6.2f\n",
              i, 100*eval.first, eval.second, norm_u, norm_m, samples_per_sec);
    }
}

#endif

int main(int argc, char *argv[])
{
    assert(argv[1] && argv[2] && "filename missing");
    Eigen::initParallel();
    Eigen::setNbThreads(1);

    loadMarket(M, argv[1]);
    loadMarket(P, argv[2]);

    init();
#ifdef TEST_SAMPLE
    test();
#else
    run();
#endif

    return 0;
}
