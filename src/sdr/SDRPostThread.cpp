#include "SDRPostThread.h"
#include "CubicSDRDefs.h"
#include "CubicSDR.h"

#include <vector>
#include <deque>

SDRPostThread::SDRPostThread() : IOThread(), buffers("SDRPostThreadBuffers"), visualDataBuffers("SDRPostThreadVisualDataBuffers"), frequency(0) {
    iqDataInQueue = NULL;
    iqDataOutQueue = NULL;
    iqVisualQueue = NULL;

    numChannels = 0;
    channelizer = NULL;
    
    sampleRate = 0;
    nRunDemods = 0;
    
    visFrequency.store(0);
    visBandwidth.store(0);
    
    doRefresh.store(false);
    dcFilter = iirfilt_crcf_create_dc_blocker(0.0005);
}

SDRPostThread::~SDRPostThread() {
}

void SDRPostThread::bindDemodulator(DemodulatorInstance *demod) {
    busy_demod.lock();
    demodulators.push_back(demod);
    doRefresh.store(true);
    busy_demod.unlock();
}

void SDRPostThread::removeDemodulator(DemodulatorInstance *demod) {
    if (!demod) {
        return;
    }

    busy_demod.lock();
    std::vector<DemodulatorInstance *>::iterator i = std::find(demodulators.begin(), demodulators.end(), demod);
    
    if (i != demodulators.end()) {
        demodulators.erase(i);
        doRefresh.store(true);
    }
    busy_demod.unlock();
}

void SDRPostThread::initPFBChannelizer() {
//    std::cout << "Initializing post-process FIR polyphase filterbank channelizer with " << numChannels << " channels." << std::endl;
    if (channelizer) {
        firpfbch_crcf_destroy(channelizer);
    }
    channelizer = firpfbch_crcf_create_kaiser(LIQUID_ANALYZER, numChannels, 4, 60);
    
    chanBw = (sampleRate / numChannels);
    
    chanCenters.resize(numChannels+1);
    demodChannelActive.resize(numChannels+1);
    
//    std::cout << "Channel bandwidth spacing: " << (chanBw) << std::endl;
}

void SDRPostThread::updateActiveDemodulators() {
    // In range?
    std::vector<DemodulatorInstance *>::iterator demod_i;
    
    nRunDemods = 0;
    
    long long centerFreq = wxGetApp().getFrequency();
    
    for (demod_i = demodulators.begin(); demod_i != demodulators.end(); demod_i++) {
        DemodulatorInstance *demod = *demod_i;
        DemodulatorThreadInputQueue *demodQueue = demod->getIQInputDataPipe();
        
        // not in range?
        if (demod->isDeltaLock()) {
            if (demod->getFrequency() != centerFreq + demod->getDeltaLockOfs()) {
                demod->setFrequency(centerFreq + demod->getDeltaLockOfs());
                demod->updateLabel(demod->getFrequency());
                demod->setFollow(false);
                demod->setTracking(false);
            }
        }
        
        if (abs(frequency - demod->getFrequency()) > (sampleRate / 2)) {
            // deactivate if active
            if (demod->isActive() && !demod->isFollow() && !demod->isTracking()) {
                demod->setActive(false);
                DemodulatorThreadIQData *dummyDataOut = new DemodulatorThreadIQData;
                dummyDataOut->frequency = frequency;
                dummyDataOut->sampleRate = sampleRate;
                demodQueue->push(dummyDataOut);
            }
            
            // follow if follow mode
            if (demod->isFollow() && centerFreq != demod->getFrequency()) {
                wxGetApp().setFrequency(demod->getFrequency());
                demod->setFollow(false);
            }
        } else if (!demod->isActive()) { // in range, activate if not activated
            demod->setActive(true);
            if (wxGetApp().getDemodMgr().getLastActiveDemodulator() == NULL) {
                wxGetApp().getDemodMgr().setActiveDemodulator(demod);
            }
        }
        
        if (!demod->isActive()) {
            continue;
        }
        
        // Add to the current run
        if (nRunDemods == runDemods.size()) {
            runDemods.push_back(demod);
            demodChannel.push_back(-1);
        } else {
            runDemods[nRunDemods] = demod;
            demodChannel[nRunDemods] = -1;
        }
        nRunDemods++;
    }
}

void SDRPostThread::updateChannels() {
    // calculate channel center frequencies, todo: cache
    for (int i = 0; i < numChannels/2; i++) {
        int ofs = ((chanBw) * i);
        chanCenters[i] = frequency + ofs;
        chanCenters[i+(numChannels/2)] = frequency - (sampleRate/2) + ofs;
    }
    chanCenters[numChannels] = frequency + (sampleRate/2);
}

int SDRPostThread::getChannelAt(long long frequency) {
    int chan = -1;
    long long minDelta = sampleRate;
    for (int i = 0; i < numChannels+1; i++) {
        long long fdelta = abs(frequency - chanCenters[i]);
        if (fdelta < minDelta) {
            minDelta = fdelta;
            chan = i;
        }
    }
    return chan;
}

void SDRPostThread::setIQVisualRange(long long frequency, int bandwidth) {
    visFrequency.store(frequency);
    visBandwidth.store(bandwidth);
}

void SDRPostThread::run() {
#ifdef __APPLE__
    pthread_t tID = pthread_self();  // ID of this thread
    int priority = sched_get_priority_max( SCHED_FIFO);
    sched_param prio = {priority}; // scheduling priority of thread
    pthread_setschedparam(tID, SCHED_FIFO, &prio);
#endif

    std::cout << "SDR post-processing thread started.." << std::endl;

    iqDataInQueue = (SDRThreadIQDataQueue*)getInputQueue("IQDataInput");
    iqDataOutQueue = (DemodulatorThreadInputQueue*)getOutputQueue("IQDataOutput");
    iqVisualQueue = (DemodulatorThreadInputQueue*)getOutputQueue("IQVisualDataOutput");
    iqActiveDemodVisualQueue = (DemodulatorThreadInputQueue*)getOutputQueue("IQActiveDemodVisualDataOutput");

    iqDataInQueue->set_max_num_items(0);
    
    while (!terminated) {
        SDRThreadIQData *data_in;
        
        iqDataInQueue->pop(data_in);
        //        std::lock_guard < std::mutex > lock(data_in->m_mutex);

        busy_demod.lock();

        if (data_in && data_in->data.size()) {
            if(data_in->numChannels > 1) {
                runPFBCH(data_in);
            } else {
                runSingleCH(data_in);
            }
        }

        data_in->decRefCount();

        bool doUpdate = false;
        for (size_t j = 0; j < nRunDemods; j++) {
            DemodulatorInstance *demod = runDemods[j];
            if (abs(frequency - demod->getFrequency()) > (sampleRate / 2)) {
                doUpdate = true;
            }
        }
        
        if (doUpdate) {
            updateActiveDemodulators();
        }
        
        busy_demod.unlock();
    }
    
    if (iqVisualQueue && !iqVisualQueue->empty()) {
        DemodulatorThreadIQData *visualDataDummy;
        iqVisualQueue->pop(visualDataDummy);
    }

    //    buffers.purge();
    //    visualDataBuffers.purge();

    std::cout << "SDR post-processing thread done." << std::endl;
}

void SDRPostThread::terminate() {
    terminated = true;
    SDRThreadIQData *dummy = new SDRThreadIQData;
    iqDataInQueue->push(dummy);
}

void SDRPostThread::runSingleCH(SDRThreadIQData *data_in) {
    if (sampleRate != data_in->sampleRate) {
        sampleRate = data_in->sampleRate;
        numChannels = 1;
        doRefresh.store(true);
    }
    
    size_t dataSize = data_in->data.size();
    size_t outSize = data_in->data.size();
    
    if (outSize > dataOut.capacity()) {
        dataOut.reserve(outSize);
    }
    if (outSize != dataOut.size()) {
        dataOut.resize(outSize);
    }
    
    if (frequency != data_in->frequency) {
        frequency = data_in->frequency;
        doRefresh.store(true);
    }
    
    if (doRefresh.load()) {
        updateActiveDemodulators();
        doRefresh.store(false);
    }
    
    size_t refCount = nRunDemods;
    bool doIQDataOut = (iqDataOutQueue != NULL && !iqDataOutQueue->full());
    bool doDemodVisOut = (nRunDemods && iqActiveDemodVisualQueue != NULL && !iqActiveDemodVisualQueue->full());
    bool doVisOut = (iqVisualQueue != NULL && !iqVisualQueue->full());
    
    if (doIQDataOut) {
        refCount++;
    }
    if (doDemodVisOut) {
        refCount++;
    }
    if (doVisOut) {
        refCount++;
    }
    
    if (refCount) {
        DemodulatorThreadIQData *demodDataOut = buffers.getBuffer();
        demodDataOut->setRefCount(refCount);
        demodDataOut->frequency = frequency;
        demodDataOut->sampleRate = sampleRate;
        
        if (demodDataOut->data.size() != dataSize) {
            if (demodDataOut->data.capacity() < dataSize) {
                demodDataOut->data.reserve(dataSize);
            }
            demodDataOut->data.resize(dataSize);
        }
        
        iirfilt_crcf_execute_block(dcFilter, &data_in->data[0], dataSize, &demodDataOut->data[0]);

        if (doDemodVisOut) {
            iqActiveDemodVisualQueue->push(demodDataOut);
        }
        
        if (doIQDataOut) {
            iqDataOutQueue->push(demodDataOut);
        }

        if (doVisOut) {
            iqVisualQueue->push(demodDataOut);
        }
        
        for (size_t i = 0; i < nRunDemods; i++) {
            runDemods[i]->getIQInputDataPipe()->push(demodDataOut);
        }
    }
}

void SDRPostThread::runPFBCH(SDRThreadIQData *data_in) {
    if (numChannels != data_in->numChannels || sampleRate != data_in->sampleRate) {
        numChannels = data_in->numChannels;
        sampleRate = data_in->sampleRate;
        initPFBChannelizer();
        doRefresh.store(true);
    }
    
    size_t dataSize = data_in->data.size();
    size_t outSize = data_in->data.size();
    
    if (outSize > dataOut.capacity()) {
        dataOut.reserve(outSize);
    }
    if (outSize != dataOut.size()) {
        dataOut.resize(outSize);
    }
    
    if (iqDataOutQueue != NULL && !iqDataOutQueue->full()) {
        DemodulatorThreadIQData *iqDataOut = visualDataBuffers.getBuffer();
        
        bool doVis = false;
        
        if (iqVisualQueue != NULL && !iqVisualQueue->full()) {
            doVis = true;
        }
        
        iqDataOut->setRefCount(1 + (doVis?1:0));
        
        iqDataOut->frequency = data_in->frequency;
        iqDataOut->sampleRate = data_in->sampleRate;
        iqDataOut->data.assign(data_in->data.begin(), data_in->data.begin() + dataSize);
        
        iqDataOutQueue->push(iqDataOut);
        if (doVis) {
            iqVisualQueue->push(iqDataOut);
        }
    }
    
    if (frequency != data_in->frequency) {
        frequency = data_in->frequency;
        doRefresh.store(true);
    }
    
    if (doRefresh.load()) {
        updateActiveDemodulators();
        updateChannels();
        doRefresh.store(false);
    }
    
    DemodulatorInstance *activeDemod = wxGetApp().getDemodMgr().getLastActiveDemodulator();
    int activeDemodChannel = -1;
    
    // Find active demodulators
    if (nRunDemods) {
        
        // channelize data
        // firpfbch output rate is (input rate / channels)
        for (int i = 0, iMax = dataSize; i < iMax; i+=numChannels) {
            firpfbch_crcf_analyzer_execute(channelizer, &data_in->data[i], &dataOut[i]);
        }
        
        for (int i = 0, iMax = numChannels+1; i < iMax; i++) {
            demodChannelActive[i] = 0;
        }
        
        // Find nearest channel for each demodulator
        for (size_t i = 0; i < nRunDemods; i++) {
            DemodulatorInstance *demod = runDemods[i];
            demodChannel[i] = getChannelAt(demod->getFrequency());
            if (demod == activeDemod) {
                activeDemodChannel = demodChannel[i];
            }
        }
        
        for (size_t i = 0; i < nRunDemods; i++) {
            // cache channel usage refcounts
            if (demodChannel[i] >= 0) {
                demodChannelActive[demodChannel[i]]++;
            }
        }
        
        // Run channels
        for (int i = 0; i < numChannels+1; i++) {
            int doDemodVis = ((activeDemodChannel == i) && (iqActiveDemodVisualQueue != NULL) && !iqActiveDemodVisualQueue->full())?1:0;
            
            if (!doDemodVis && demodChannelActive[i] == 0) {
                continue;
            }
            
            DemodulatorThreadIQData *demodDataOut = buffers.getBuffer();
            demodDataOut->setRefCount(demodChannelActive[i] + doDemodVis);
            demodDataOut->frequency = chanCenters[i];
            demodDataOut->sampleRate = chanBw;
            
            // Calculate channel buffer size
            size_t chanDataSize = (outSize/numChannels);
            
            if (demodDataOut->data.size() != chanDataSize) {
                if (demodDataOut->data.capacity() < chanDataSize) {
                    demodDataOut->data.reserve(chanDataSize);
                }
                demodDataOut->data.resize(chanDataSize);
            }
            
            int idx = i;
            
            // Extra channel wraps lower side band of lowest channel
            // to fix frequency gap on upper side of spectrum
            if (i == numChannels) {
                idx = (numChannels/2);
            }
            
            // prepare channel data buffer
            if (i == 0) {   // Channel 0 requires DC correction
                if (dcBuf.size() != chanDataSize) {
                    dcBuf.resize(chanDataSize);
                }
                for (size_t j = 0; j < chanDataSize; j++) {
                    dcBuf[j] = dataOut[idx];
                    idx += numChannels;
                }
                iirfilt_crcf_execute_block(dcFilter, &dcBuf[0], chanDataSize, &demodDataOut->data[0]);
            } else {
                for (size_t j = 0; j < chanDataSize; j++) {
                    demodDataOut->data[j] = dataOut[idx];
                    idx += numChannels;
                }
            }
            
            if (doDemodVis) {
                iqActiveDemodVisualQueue->push(demodDataOut);
            }
            
            for (size_t j = 0; j < nRunDemods; j++) {
                if (demodChannel[j] == i) {
                    DemodulatorInstance *demod = runDemods[j];
                    demod->getIQInputDataPipe()->push(demodDataOut);
                }
            }
        }
    }
}
