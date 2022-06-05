#ifndef RATICATE_UNKNOWNMATRIX_HPP
#define RATICATE_UNKNOWNMATRIX_HPP

#include "Rcpp.h"
#include "tatami/tatami.hpp"
#include <vector>
#include <memory>
#include <string>
#include <stdexcept>

namespace raticate {

template<typename Data, typename Index>
class UnknownMatrix : public tatami::Matrix<Data, Index> {
public: 
    UnknownMatrix(Rcpp::RObject seed) :
        original_seed(seed),
        delayed_env(Rcpp::Environment::namespace_env("DelayedArray")),
        dense_extractor(delayed_env["extract_array"]),
        sparse_extractor(delayed_env["extract_sparse_array"])
    {
        {
            auto base = Rcpp::Environment::base_env();
            Rcpp::Function fun = base["dim"];
            Rcpp::RObject output = fun(seed);
            if (output.sexp_type() != INTSXP) {
                throw std::runtime_error("'dims' should return an integer vector");
            }
            Rcpp::IntegerVector dims(output);
            if (dims.size() != 2 || dims[0] < 0 || dims[1] < 0) {
                throw std::runtime_error("'dims' should contain two non-negative integers");
            }
            nrow_ = dims[0];
            ncol_ = dims[1];
        }

        {
            auto bcg_env = Rcpp::Environment::namespace_env("BiocGenerics");
            Rcpp::Function fun = bcg_env["type"];
            Rcpp::CharacterVector type = fun(seed);
            if (type.size() != 1) {
                throw std::runtime_error("'type' should return a character vector of length 1");
            }

            std::string type_str = Rcpp::as<std::string>(type[0]);
            if (type_str == std::string("logical")) {
                type_ = 0;      
            } else if (type_str == std::string("integer")) {
                type_ = 1;
            } else {
                type_ = 2;
            }
        }

        {
            Rcpp::Function fun = delayed_env["is_sparse"];
            Rcpp::LogicalVector sparse = fun(seed);
            if (sparse.size() != 1) {
                throw std::runtime_error("'type' should return a logical vector of length 1");
            }
            sparse_ = (sparse[0] != 0);
        }

        {
            Rcpp::Function fun = delayed_env["chunkdim"];
            Rcpp::RObject output = fun(seed);
            needs_chunks = !output.isNULL();
            if (needs_chunks) {
                Rcpp::IntegerVector chunks(output);
                if (chunks.size() != 2 || chunks[0] < 0 || chunks[1] < 0) {
                    throw std::runtime_error("'chunks' should contain two non-negative integers");
                }
                chunk_nrow = chunks[0];
                chunk_ncol = chunks[1];
            }
        }

        {
            Rcpp::Function fun = delayed_env["colAutoGrid"];
            Rcpp::RObject output = fun(seed);
            Rcpp::IntegerVector spacing = output.slot("spacings");
            if (spacing.size() != 2 || spacing[1] < 0) {
                throw std::runtime_error("'spacings' slot of 'colAutoGrid' output should contain two non-negative integers");
            }
            block_ncol = spacing[1];
        }

        {
            Rcpp::Function fun = delayed_env["rowAutoGrid"];
            Rcpp::RObject output = fun(seed);
            Rcpp::IntegerVector spacing = output.slot("spacings");
            if (spacing.size() != 2 || spacing[0] < 0) {
                throw std::runtime_error("'spacings' slot of 'rowAutoGrid' output should contain two non-negative integers");
            }
            block_nrow = spacing[0];
        }
    }

public:
    size_t nrow() const {
        return nrow_;
    }

    size_t ncol() const {
        return ncol_;
    }

    bool sparse() const {
        return sparse_;
    }

    bool prefer_rows() const {
        // All of the individual extract_array outputs are effectively column-major.
        return false;
    }

public:
    struct UnknownWorkspace : public tatami::Workspace {
        UnknownWorkspace(bool r = true) : byrow(r) {}
        bool byrow;
        size_t primary_block_start, primary_block_end;
        size_t secondary_chunk_start, secondary_chunk_end;
        std::shared_ptr<tatami::Matrix<Data, Index> > buffer = nullptr;
    };

    std::shared_ptr<tatami::Workspace> new_workspace(bool row) const { 
        return std::shared_ptr<tatami::Workspace>(new UnknownWorkspace(row));
    }

public:
    const Data* row(size_t r, Data* buffer, size_t first, size_t last, tatami::Workspace* work=nullptr) const {
        if (work == NULL) {
            quick_dense_extractor<true>(r, buffer, first, last);
            return buffer;
        } else {
            buffered_dense_extractor<true>(r, buffer, first, last, work);
            return buffer;
        }
    }

    const Data* column(size_t c, Data* buffer, size_t first, size_t last, tatami::Workspace* work=nullptr) const {
        if (work == NULL) {
            quick_dense_extractor<false>(c, buffer, first, last);
            return buffer;
        } else {
            buffered_dense_extractor<false>(c, buffer, first, last, work);
            return buffer;
        }
    } 

private:
    Rcpp::RObject create_index_vector(size_t first, size_t last, size_t max) const {
        if (first != 0 || last != max) {
            Rcpp::IntegerVector alt(last - first);
            std::iota(alt.begin(), alt.end(), first + 1); // 1-based.
            return alt;
        } else {
            return R_NilValue;
        }
    }

    std::pair<size_t, size_t> round_indices(size_t first, size_t last, size_t interval, size_t max) const {
        size_t new_first = (first / interval) * interval;
        size_t new_last = std::min(
            max, 
            (last ? 
                0 : 
                ((last - 1) / interval + 1) * interval // i.e., ceil(last/interval) * interval.
            )
        );
        return std::make_pair(new_first, new_last);
    }

private:
    template<bool byrow>
    void quick_dense_extractor(size_t i, Data* buffer, size_t first, size_t last) const {
        Rcpp::List indices(2);
        indices[(byrow ? 0 : 1)] = Rcpp::IntegerVector::create(i + 1);
        indices[(byrow ? 1 : 0)] = create_index_vector(first, last, (byrow ? ncol_ : nrow_));

        auto val0 = dense_extractor(original_seed, indices);
        if (type_ == 0) {
            Rcpp::LogicalVector val(val0);
            std::copy(val.begin(), val.end(), buffer);
        } else if (type_ == 1) {
            Rcpp::IntegerVector val(val0);
            std::copy(val.begin(), val.end(), buffer);
        } else {
            Rcpp::NumericVector val(val0);
            std::cout << val.size() << std::endl;
            std::copy(val.begin(), val.end(), buffer);
        }
    }

    template<bool byrow>
    void buffered_dense_extractor(size_t i, Data* buffer, size_t first, size_t last, tatami::Workspace* work0) const {
        UnknownWorkspace* work = static_cast<UnknownWorkspace*>(work0);
        if (work->byrow != byrow) {
            throw std::runtime_error("workspace should have been generated with 'row=" + std::to_string(byrow) + "'");
        }

        bool reset = true;
        if (work->buffer != nullptr) {
            if (i >= work->primary_block_start && i < work->primary_block_end) {
                if (first >= work->secondary_chunk_start && last <= work->secondary_chunk_end) {
                    reset = false;
                }
            }
        }

        if (reset) {
            Rcpp::List indices(2);
            if constexpr(byrow) {
                auto row_rounded = round_indices(i, i + 1, block_nrow, nrow_);
                indices[0] = create_index_vector(row_rounded.first, row_rounded.second, nrow_);
                work->primary_block_start = row_rounded.first;
                work->primary_block_end = row_rounded.second;

                auto col_rounded = (needs_chunks ? round_indices(first, last, chunk_ncol, ncol_) : std::make_pair(first, last));
                indices[1] = create_index_vector(col_rounded.first, col_rounded.second, ncol_);
                work->secondary_chunk_start = col_rounded.first;
                work->secondary_chunk_end = col_rounded.second;

            } else {
                auto row_rounded = (needs_chunks ? round_indices(first, last, chunk_nrow, nrow_) : std::make_pair(first, last));
                indices[0] = create_index_vector(row_rounded.first, row_rounded.second, nrow_);
                work->secondary_chunk_start = row_rounded.first;
                work->secondary_chunk_end = row_rounded.second;

                auto col_rounded = round_indices(i, i + 1, block_ncol, ncol_);
                indices[1] = create_index_vector(col_rounded.first, col_rounded.second, ncol_);
                work->primary_block_start = col_rounded.first;
                work->primary_block_end = col_rounded.second;
            }

            auto val0 = dense_extractor(original_seed, indices);
            if (type_ == 0) {
                Rcpp::LogicalMatrix val(val0);
                std::vector<int> holding(val.begin(), val.end());
                (work->buffer).reset(new tatami::DenseColumnMatrix<Data, Index, decltype(holding)>(val.rows(), val.cols(), std::move(holding)));
            } else if (type_ == 1) {
                Rcpp::IntegerMatrix val(val0);
                std::vector<int> holding(val.begin(), val.end());
                (work->buffer).reset(new tatami::DenseColumnMatrix<Data, Index, decltype(holding)>(val.rows(), val.cols(), std::move(holding)));
            } else {
                Rcpp::NumericMatrix val(val0);
                std::vector<double> holding(val.begin(), val.end());
                (work->buffer).reset(new tatami::DenseColumnMatrix<Data, Index, decltype(holding)>(val.rows(), val.cols(), std::move(holding)));
            }
        }

        if constexpr(byrow) {
            (work->buffer)->row_copy(i - work->primary_block_start, buffer, first - work->secondary_chunk_start, last - work->secondary_chunk_end);
        } else {
            (work->buffer)->column_copy(i - work->primary_block_start, buffer, first - work->secondary_chunk_start, last - work->secondary_chunk_end);
        }
    }

private:
    size_t nrow_, ncol_;
    bool sparse_;
    int type_;

    bool needs_chunks;
    size_t chunk_nrow, chunk_ncol;

    size_t block_nrow, block_ncol;

private:
    Rcpp::RObject original_seed;
    Rcpp::Environment delayed_env;
    Rcpp::Function dense_extractor, sparse_extractor;
};

}

#endif