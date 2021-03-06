#pragma once

#include <cudnn.h>
#include <zi/time.hpp>

#include "cuda_utils.hpp"
#include "../types.hpp"
#include "../assert.hpp"
#include "../init.hpp"


namespace znn { namespace fwd { namespace gpu3d {

class gpu_layer
{
public:
    virtual ~gpu_layer() {}

    virtual void forward( float *, float * ) = 0;
    virtual int in_memory()  const = 0;
    virtual int out_memory() const = 0;
};

class pooling_layer: public gpu_layer
{
private:
    cudnnTensorDescriptor_t in_desc;
    cudnnTensorDescriptor_t out_desc;

    cudnnPoolingDescriptor_t pooling_desc;

    int in_memory_;
    int out_memory_;

    vec3i     is_;
    vec3i     fs_;
    ptrdiff_t delta_;

    cudnnHandle_t& cudnn_handle_;

public:

    void forward( float * in, float * out ) override
    {
        float * eout = out;

        float alpha = 1;
        float beta  = 0;

        for ( int x = 0; x < fs_[0]; ++x )
            for ( int y = 0; y < fs_[1]; ++y )
                for ( int z = 0; z < fs_[2]; ++z )
                {
                    checkCUDNN( cudnnPoolingForward(
                                    cudnn_handle_,
                                    pooling_desc,
                                    &alpha, in_desc,
                                    in + x * is_[1] * is_[2] + y * is_[2] + z,
                                    &beta, out_desc, eout) );

                    eout += delta_;
                }
    }

    int in_memory() const override
    {
        return in_memory_;
    }

    int out_memory() const override
    {
        return out_memory_;
    }


    ~pooling_layer()
    {
        checkCUDNN( cudnnDestroyPoolingDescriptor(pooling_desc) );
        checkCUDNN( cudnnDestroyTensorDescriptor(in_desc) );
        checkCUDNN( cudnnDestroyTensorDescriptor(out_desc) );
    }

    pooling_layer( cudnnHandle_t & cudnn_handle,
                   int n, int c,
                   vec3i const & is,
                   vec3i const & fs )
        : is_(is)
        , fs_(fs)
        , cudnn_handle_(cudnn_handle)
    {
        int n_out = n * fs[0] * fs[1] * fs[2];

        STRONG_ASSERT( (is+vec3i::one) % fs == vec3i::zero );

        vec3i os  = is / fs;
        vec3i eis = os * fs;

        in_memory_  = n     * c * is[0] * is[1] * is[2] * sizeof(float);
        out_memory_ = n_out * c * os[0] * os[1] * os[2] * sizeof(float);

        checkCUDNN( cudnnCreateTensorDescriptor(&in_desc) );

        {
            int dims[5]    = {n,c,eis[0],eis[1],eis[2]};
            int strides[5] = {c*is[0]*is[1]*is[2],
                              is[0]*is[1]*is[2],
                              is[1]*is[2],
                              is[2],
                              1};

            checkCUDNN( cudnnSetTensorNdDescriptor(
                            in_desc,
                            CUDNN_DATA_FLOAT,
                            5, dims, strides) );
        }

        checkCUDNN( cudnnCreateTensorDescriptor(&out_desc) );
        {
            int dims[5]    = {n,c,os[0],os[1],os[2]};
            int strides[5] = {c*os[0]*os[1]*os[2],
                              os[0]*os[1]*os[2],
                              os[1]*os[2],
                              os[2],
                              1};

            delta_ = n * c * os[0] * os[1] * os[2];

            checkCUDNN( cudnnSetTensorNdDescriptor(
                            out_desc,
                            CUDNN_DATA_FLOAT,
                            5, dims, strides) );
        }

        checkCUDNN( cudnnCreatePoolingDescriptor(&pooling_desc) );

        {
            int window[3] = {fs[0],fs[1],fs[2]};
            int padding[3] = {0,0,0};

            checkCUDNN( cudnnSetPoolingNdDescriptor(
                            pooling_desc,
                            CUDNN_POOLING_MAX,
                            3, window, padding, window ));
        }

    }
};


class conv_layer: public gpu_layer
{
private:
    cudnnTensorDescriptor_t      in_desc, out_desc, bias_desc;
    cudnnFilterDescriptor_t      filter_desc;
    cudnnConvolutionDescriptor_t conv_desc;

    int in_memory_ ;
    int out_memory_;

    float * filter_data_ ;
    float * bias_data_   ;

    size_t workspace_size_ = 0;

    cudnnHandle_t& cudnn_handle_;

public:

    float* filter_data()
    {
        return filter_data_;
    }

    float* bias_data()
    {
        return bias_data_;
    }

    void forward( float * in, float * out ) override
    {
        void * workspace = NULL;

        if ( workspace_size_ )
        {
            checkCudaErrors( cudaMalloc(&workspace, workspace_size_ ));
        }

        std::cout << "Workspace: " << ( workspace_size_ / 1024 / 1024 )
                  << " MB\n";

        float alpha = 1; float beta = 0;

        checkCUDNN(
            cudnnConvolutionForward(
                cudnn_handle_,
                &alpha,
                in_desc, in,
                filter_desc, filter_data_,
                conv_desc,
#if defined(ZNN_NO_PRECOMP_GEMM)
                CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM,
#else
                CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_PRECOMP_GEMM,
#endif
                workspace, workspace_size_,
                &beta,
                out_desc, out) );

        if ( workspace_size_ )
        {
            checkCudaErrors( cudaFree(workspace) );
        }

        beta = 1;

        checkCUDNN(
            cudnnAddTensor( cudnn_handle_,
                            &alpha,
                            bias_desc, bias_data_,
                            &beta,
                            out_desc, out) );
        beta = 0;

        checkCUDNN(
            cudnnActivationForward(
                cudnn_handle_,
                CUDNN_ACTIVATION_RELU,
                &alpha, out_desc, out,
                &beta, out_desc, out) );

    }

    int in_memory() const override
    {
        return in_memory_;
    }

    int out_memory() const override
    {
        return out_memory_;
    }


    ~conv_layer()
    {
        checkCudaErrors( cudaFree(filter_data_) );
        checkCudaErrors( cudaFree(bias_data_) );

        checkCUDNN( cudnnDestroyTensorDescriptor(in_desc) );
        checkCUDNN( cudnnDestroyTensorDescriptor(out_desc) );

        checkCUDNN( cudnnDestroyTensorDescriptor(bias_desc) );
        checkCUDNN( cudnnDestroyFilterDescriptor(filter_desc) );

        checkCUDNN( cudnnDestroyConvolutionDescriptor(conv_desc) );
    }

private:
    void create_tensor_descriptor( cudnnTensorDescriptor_t * descriptor,
                                   int n, int c, int d, int h, int w )
    {
        checkCUDNN( cudnnCreateTensorDescriptor(descriptor) );

        int dims[5] = {n,c,d,h,w};
        int strides[5] = {c*d*h*w,d*h*w,h*w,w,1};
        checkCUDNN(
            cudnnSetTensorNdDescriptor(*descriptor,
                                       CUDNN_DATA_FLOAT,
                                       5, dims, strides));
    }

public:
    conv_layer( cudnnHandle_t& cudnn_handle,
                int n, int fin, int fout,
                vec3i const & is,
                vec3i const & fs )
        : cudnn_handle_(cudnn_handle)
    {
        size_t filter_memory
            = fin * fout * fs[0] * fs[1] * fs[2] * sizeof(float);
        size_t bias_memory
            = fout * sizeof(float);

        checkCudaErrors( cudaMalloc(&filter_data_, filter_memory ));
        checkCudaErrors( cudaMalloc(&bias_data_  , bias_memory   ));

        vec3i os = is + vec3i::one - fs;

        create_tensor_descriptor(&in_desc,n,fin,is[0],is[1],is[2]);
        create_tensor_descriptor(&out_desc,n,fout,os[0],os[1],os[2]);
        create_tensor_descriptor(&bias_desc,1,fout,1,1,1);

        checkCUDNN( cudnnCreateFilterDescriptor(&filter_desc) );
        {
            int dims[5] = {fout,fin,fs[0],fs[1],fs[2]};
            checkCUDNN(
                cudnnSetFilterNdDescriptor(filter_desc,
                                           CUDNN_DATA_FLOAT,
                                           5, dims));
        }

        checkCUDNN( cudnnCreateConvolutionDescriptor(&conv_desc) );
        {
            int pad[3] = {0,0,0};
            int ones[3] = {1,1,1};

            checkCUDNN(
                cudnnSetConvolutionNdDescriptor(
                    conv_desc,
                    3, pad, ones, ones,
                    CUDNN_CONVOLUTION,
                    //CUDNN_CROSS_CORRELATION,
                    CUDNN_DATA_FLOAT) );

        }

        in_memory_  = n * fin * is[0] * is[1] * is[2] * sizeof(float);
        out_memory_ = n * fout * os[0] * os[1] * os[2] * sizeof(float);

#if !defined(ZNN_NO_PRECOMP_GEMM)
        {
            size_t what_size;
            checkCUDNN(
                cudnnGetConvolutionForwardWorkspaceSize(
                    cudnn_handle,
                    in_desc,
                    filter_desc,
                    conv_desc,
                    out_desc,
                    CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM,
                    &what_size));

            workspace_size_ = std::max(workspace_size_, what_size);
        }
#endif
    }
};


class fwd_network
{
private:
    struct layer_descriptor
    {
        int layer_type   ;
        int fin          ;
        int fout         ;
        vec3i filter_size;

        layer_descriptor(int l, int a, int b, vec3i const & s)
            : layer_type(l)
            , fin(a)
            , fout(b)
            , filter_size(s)
        {}
    };

    std::list<layer_descriptor> descriptors;
    std::list<gpu_layer*>       layers     ;

    int num_input_units;
    int curr_num_units;

    vec3i current_dilation = vec3i::one;
    vec3i out_size;
    vec3i in_size;

    int batch_size;

    cudnnHandle_t cudnn_handle;

    int num_output_floats;

public:

    ~fwd_network()
    {
        checkCUDNN( cudnnDestroy(cudnn_handle) );
        for ( auto & l: layers )
        {
            delete l;
        }
    }

    fwd_network( int n )
        : num_input_units(n)
        , curr_num_units(n)
    {
        checkCUDNN( cudnnCreate(&cudnn_handle) );
    }

    fwd_network & conv( int c, vec3i const & w )
    {
        descriptors.push_front(layer_descriptor(1,curr_num_units,c,w));
        curr_num_units = c;
        return *this;
    }

    fwd_network & pool( vec3i const & w )
    {
        descriptors.push_front(layer_descriptor(2,curr_num_units,curr_num_units,w));
        current_dilation *= w;
        return *this;
    }

    void done( int n, vec3i const & sz )
    {
        num_output_floats = n * sz[0] * sz[1] * sz[2] * curr_num_units;;

        STRONG_ASSERT( (sz % current_dilation) == vec3i::zero );

        batch_size = n * current_dilation[0] *
            current_dilation[1] * current_dilation[2];

        out_size   = sz / current_dilation;

        uniform_init ui(0.1);

        for ( auto & d: descriptors )
        {
            if ( d.layer_type == 1 )
            {
                in_size = out_size + d.filter_size - vec3i::one;

                conv_layer* cl = new conv_layer(cudnn_handle, batch_size,
                                                d.fin, d.fout,
                                                in_size, d.filter_size);

                int floats = d.fin * d.fout *
                    d.filter_size[0] * d.filter_size[1] * d.filter_size[2];
                float* dt = new float[floats];
                ui.initialize(dt, floats);

                checkCudaErrors( cudaMemcpy(cl->filter_data(), dt,
                                            floats * sizeof(float),
                                            cudaMemcpyHostToDevice) );

                delete[] dt;

                dt = new float[d.fout];
                ui.initialize(dt, d.fout);

                checkCudaErrors( cudaMemcpy(cl->bias_data(), dt,
                                            d.fout * sizeof(float),
                                            cudaMemcpyHostToDevice) );

                delete[] dt;

                layers.push_front(cl);

                std::cout << "Conv: " << batch_size << "  \t"
                          << d.fin << '\t'
                          << d.fout << '\t'
                          << in_size << '\t'
                          << out_size << '\t'
                          << d.filter_size << '\n';

                out_size = in_size;
            }
            else if ( d.layer_type == 2 )
            {
                in_size = out_size * d.filter_size + d.filter_size - 1;
                batch_size /=
                    d.filter_size[0] * d.filter_size[1] * d.filter_size[2];

                pooling_layer* pl = new pooling_layer(cudnn_handle, batch_size,
                                                      d.fin, in_size,
                                                      d.filter_size);

                layers.push_front(pl);

                std::cout << "Pool: " << batch_size << "  \t"
                          << d.fin << '\t'
                          << d.fin << '\t'
                          << in_size << '\t'
                          << out_size << '\t'
                          << d.filter_size << '\n';

                out_size = in_size;
            }
        }

    }

    void benchmark( int rounds )
    {
        uniform_init ui(0.1);

        int host_data_len = layers.front()->in_memory()/sizeof(float);
        int host_out_len  = num_output_floats;

        float* host_data_in = new float[host_data_len];
        float* host_data_out = new float[host_out_len];

        zi::wall_timer wt;

        for ( ; rounds > 0; --rounds )
        {

            ui.initialize(host_data_in, host_data_len);

            wt.reset();

            float* input;
            float* output;

            checkCudaErrors( cudaMalloc(&input, layers.front()->in_memory()) );

            checkCudaErrors( cudaMemcpy(input, host_data_in,
                                        layers.front()->in_memory(),
                                        cudaMemcpyHostToDevice) );

            gpu_layer* last;

            for ( auto & a: layers )
            {

                std::cout << "INS: " << a->in_memory()
                          << " OUT: " << a->out_memory() << "\n";

                last = a;
                checkCudaErrors( cudaMalloc(&output, a->out_memory()) );

                a->forward(input, output);

                //print_free_memory();

                checkCudaErrors( cudaFree(input) );
                input = output;
            }


            std::cout << last->out_memory()
                      << ' ' << (host_out_len*4) << "\n";

            checkCudaErrors( cudaMemcpy(host_data_out, input,
                                        last->out_memory(),
                                        cudaMemcpyDeviceToHost) );

            checkCudaErrors( cudaFree(input) );

            double tm = wt.elapsed<double>();
            std::cout << tm << "\n";
            std::cout << static_cast<double>(num_output_floats/4) / tm << "\n";

        }


        delete [] host_data_in;
        delete [] host_data_out;

    }

};


}}} // namespace znn::fwd::gpu3d
