#include "signal/spike_filter.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
int main(int argc,char **argv) {
    if(argc!=7){std::cerr<<"input.f64 sample_rate events.csv negative|positive batch_size scale\n";return 2;}
    std::ifstream input(argv[1],std::ios::binary);std::ofstream output(argv[3]);
    if(!input||!output)return 3;
    ccv2::SpikeProcConfig cfg;cfg.sampleRate=std::stod(argv[2]);
    const std::string polarity(argv[4]);if(polarity!="negative"&&polarity!="positive")return 4;
    cfg.negativePolarity=(polarity=="negative");
    const int batch=std::stoi(argv[5]);const double scale=std::stod(argv[6]);if(batch<1)return 5;
    ccv2::SpikeProcessor processor;processor.configure(1,cfg);
    QVector<double> in(batch),display;QVector<bool> flags;
    std::int64_t index=0,events=0,nonfinite=0;
    output<<"sample_index\n";
    while(input){
        in.resize(batch);input.read(reinterpret_cast<char*>(in.data()),batch*sizeof(double));
        const auto bytes=input.gcount();if(bytes%sizeof(double))return 6;
        const int n=bytes/sizeof(double);if(!n)break;in.resize(n);
        for(auto &v:in)v*=scale;
        processor.processChannel(0,in,display,flags);
        if(display.size()!=n||flags.size()!=n)return 7;
        for(int i=0;i<n;++i){nonfinite+=!std::isfinite(display[i]);if(flags[i]){output<<(index+i)<<'\n';++events;}}
        index+=n;
    }
    std::cout<<std::setprecision(17)<<"{\"samples\":"<<index<<",\"events\":"<<events<<",\"nonfinite_display\":"<<nonfinite<<",\"rms\":"<<processor.currentRms(0)<<",\"threshold\":"<<processor.currentThreshold(0)<<",\"ready\":"<<(processor.detectorReady(0)?"true":"false")<<"}\n";
    return 0;
}
