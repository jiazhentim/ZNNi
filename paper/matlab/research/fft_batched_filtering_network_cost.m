function [ memory, flops ] = fft_batched_filtering_network_cost( batch, nout, layers, width, C, D )

sparse = ones(size(layers));

for x = 2:size(layers,2)
    sparse(x) = sparse(x-1);
    if layers(x) == 0
        sparse(x) = sparse(x) * 2;
    else
        layers(x) = (layers(x)-1) * sparse(x) + 1;
    end
end

nin = nout;

for x = size(layers,2):-1:1
    if layers(x) > 0
        nin = nin + layers(x) - 1;
    else
        nin = nin + 1;
    end
end

memory = 0;
flops  = 0;

for x = 1:size(layers,2)
    f0 = width;
    f1 = width;
    if x == 1
        f0 = 1;
    end
    if x == size(layers,2)
        f1 = 1;
    end

    if layers(x) > 0
        [m,f] = fft_batched_layer_cost(batch, nin, f0, f1, layers(x), C, D);
        memory = max(memory, m);
        flops  = flops  + f;
        nin = nin - layers(x) + 1;
    else
        nin = nin - 1;
        memory = max(memory, nin .* nin .* nin * batch);
        flops = flops + 3 * nin .* nin .* nin * batch;
    end
end

flops = flops ./ nout ./ nout ./ nout / batch;
memory = memory * 4; % bytes
memory = memory / 1024 / 1024 / 1024;

end
