/******************************************************************************
 * Copyright (c) 2013 Johannes Bergmann, Felix Weninger, Bjoern Schuller
 * Institute for Human-Machine Communication
 * Technische Universitaet Muenchen (TUM)
 * D-80290 Munich, Germany
 *
 * This file is part of CURRENNT.
 *
 * CURRENNT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * CURRENNT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with CURRENNT.  If not, see <http://www.gnu.org/licenses/>.
 *****************************************************************************/

#include <boost/random/uniform_int.hpp>
#include <boost/random/normal_distribution.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/filesystem.hpp>
#include <boost/thread.hpp>
#include <boost/function.hpp>

#include <thrust/functional.h>
#include <thrust/fill.h>
#include <thrust/transform.h>

#include "DataSet.hpp"
#include "../Configuration.hpp"

#include "../netcdf/netcdf.h"

#include "../helpers/misFuncs.hpp"

#include <stdexcept>
#include <algorithm>
#include <limits>
#include <cassert>

#define DATASET_EXINPUT_TYPE_0 0 // nothing
#define DATASET_EXINPUT_TYPE_1 1 // input is the index in a increasing order ([1 1 1 2..2 3..3])
                                 // other types may be implemented in the future
namespace {
namespace internal {

    int readNcDimension(int ncid, const char *dimName)
    {
        int ret;
        int dimid;
        size_t x;

        if ((ret = nc_inq_dimid(ncid, dimName, &dimid)) || (ret = nc_inq_dimlen(ncid, dimid, &x)))
            throw std::runtime_error(std::string("Cannot get dimension '") + 
				     dimName + 
				     "': "   + nc_strerror(ret));

        return (int)x;
    }

    bool hasNcDimension(int ncid, const char *dimName)
    {
        try {
            readNcDimension(ncid, dimName);
            return true;
        } 
        catch (...) {
            return false;
        }
    }

    std::string readNcStringArray(int ncid, const char *arrName, int arrIdx, int maxStringLength)
    {
        int ret;
        int varid;
        char *buffer = new char[maxStringLength+1];
        size_t start[] = {arrIdx, 0};
        size_t count[] = {1, maxStringLength};

        if ((ret = nc_inq_varid(ncid, arrName, &varid)) || 
	    (ret = nc_get_vara_text(ncid, varid, start, count, buffer)))
            throw std::runtime_error(std::string("Cannot read variable '") + 
				     arrName + "': " + nc_strerror(ret));

        buffer[maxStringLength] = '\0';
        return std::string(buffer);
    }

    int readNcIntArray(int ncid, const char *arrName, int arrIdx)
    {
        int ret;
        int varid;
        size_t start[] = {arrIdx};
        size_t count[] = {1};

        int x;
        if ((ret = nc_inq_varid(ncid, arrName, &varid)) || 
	    (ret = nc_get_vara_int(ncid, varid, start, count, &x)))
            throw std::runtime_error(std::string("Cannot read array '") + 
				     arrName + "': " + nc_strerror(ret));
        return x;
    }

    template <typename T>
    int _readNcArrayHelper(int ncid, int varid, const size_t start[], const size_t count[], T *v);

    template <>
    int _readNcArrayHelper<float>(int ncid, int varid, const size_t start[], 
				  const size_t count[], float *v)
    {
        return nc_get_vara_float(ncid, varid, start, count, v);
    }

    template <>
    int _readNcArrayHelper<double>(int ncid, int varid, const size_t start[], 
				   const size_t count[], double *v)
    {
        return nc_get_vara_double(ncid, varid, start, count, v);
    }

    template <>
    int _readNcArrayHelper<int>(int ncid, int varid, const size_t start[], 
				const size_t count[], int *v)
    {
        return nc_get_vara_int(ncid, varid, start, count, v);
    }

    template <typename T>
    thrust::host_vector<T> readNcArray(int ncid, const char *arrName, int begin, int n)
    {
        int ret;
        int varid;
        size_t start[] = {begin};
        size_t count[] = {n};

        thrust::host_vector<T> v(n);
        if ((ret = nc_inq_varid(ncid, arrName, &varid)) || 
	    (ret = _readNcArrayHelper<T>(ncid, varid, start, count, v.data())))
            throw std::runtime_error(std::string("Cannot read array '") + 
				     arrName + "': " + nc_strerror(ret));

        return v;
    }

    Cpu::real_vector readNcPatternArray(int ncid, const char *arrName, int begin, int n, 
					int patternSize)
    {
        int ret;
        int varid;
        size_t start[] = {begin, 0};
        size_t count[] = {n, patternSize};

        Cpu::real_vector v(n * patternSize);
        if ((ret = nc_inq_varid(ncid, arrName, &varid)) || 
	    (ret = _readNcArrayHelper<real_t>(ncid, varid, start, count, v.data())))
            throw std::runtime_error(std::string("Cannot read array '") + 
				     arrName + "': " + nc_strerror(ret));

        return v;
    }

    Cpu::int_vector readNcPatternArrayInt(int ncid, const char *arrName, int begin, int n, 
					  int patternSize)
    {
        int ret;
        int varid;
        size_t start[] = {begin, 0};
        size_t count[] = {n, patternSize};

        Cpu::int_vector v(n * patternSize);
        if ((ret = nc_inq_varid(ncid, arrName, &varid)) || 
	    (ret = _readNcArrayHelper<int>(ncid, varid, start, count, v.data())))
            throw std::runtime_error(std::string("Cannot read array '") + 
				     arrName + "': " + nc_strerror(ret));

        return v;
    }

    Cpu::real_vector targetClassesToOutputs(const Cpu::int_vector &targetClasses, int numLabels)
    {
        if (numLabels == 2) {
            Cpu::real_vector v(targetClasses.size());
            for (size_t i = 0; i < v.size(); ++i)
                v[i] = (real_t)targetClasses[i];

            return v;
        }
        else {
            Cpu::real_vector v(targetClasses.size() * numLabels, 0);

            for (size_t i = 0; i < targetClasses.size(); ++i)
                v[i * numLabels + targetClasses[i]] = 1;

            return v;
        }
    }
    
	    
    // Read binaryData
    int readCharData(const std::string dataPath, Cpu::pattype_vector &data)
    {
	// 
	std::ifstream ifs(dataPath.c_str(), std::ifstream::binary | std::ifstream::in);
	if (!ifs.good())
	    throw std::runtime_error(std::string("Fail to open ")+dataPath);
	
	// get the number of we data
	std::streampos numEleS, numEleE;
	long int numEle;
	numEleS = ifs.tellg();
	ifs.seekg(0, std::ios::end);
	numEleE = ifs.tellg();
	numEle  = (numEleE-numEleS)/sizeof(char);
	ifs.seekg(0, std::ios::beg);
	
	// read in the data
	data = Cpu::pattype_vector(numEle, 0);
	char tempVal;
	std::vector<char> tempVec;
	for (unsigned int i = 0; i<numEle; i++){
	    ifs.read ((char *)&tempVal, sizeof(char));
	    tempVec.push_back(tempVal);
	}
	thrust::copy(tempVec.begin(), tempVec.end(), data.begin());
	ifs.close();
	return numEle;
    }

    int readIntData(const std::string dataPath, Cpu::int_vector &data)
    {
	std::ifstream ifs(dataPath.c_str(), std::ifstream::binary | std::ifstream::in);
	if (!ifs.good())
	    throw std::runtime_error(std::string("Fail to open ")+dataPath);
	
	// get the number of we data
	std::streampos numEleS, numEleE;
	long int numEle;
	numEleS = ifs.tellg();
	ifs.seekg(0, std::ios::end);
	numEleE = ifs.tellg();
	numEle  = (numEleE-numEleS)/sizeof(int);
	ifs.seekg(0, std::ios::beg);
	
	// read in the data
	data = Cpu::int_vector(numEle, 0);
	int tempVal;
	std::vector<int> tempVec;
	for (unsigned int i = 0; i<numEle; i++){
	    ifs.read ((char *)&tempVal, sizeof(int));
	    tempVec.push_back(tempVal);
	}
	thrust::copy(tempVec.begin(), tempVec.end(), data.begin());
	ifs.close();
	return numEle;
    }
    
    int readRealData(const std::string dataPath, Cpu::real_vector &data, 
		     const int startPos, const int endPos)
    {
	// 
	std::ifstream ifs(dataPath.c_str(), std::ifstream::binary | std::ifstream::in);
	if (!ifs.good())
	    throw std::runtime_error(std::string("Fail to open ")+dataPath);
	
	// get the number of data elements
	std::streampos numEleS, numEleE;
	long int numEle;
	long int stPos, etPos;
	real_t   tempVal;

	stPos = startPos; etPos = endPos;

	numEleS = ifs.tellg();
	ifs.seekg(0, std::ios::end);
	numEleE = ifs.tellg();
	numEle  = (numEleE-numEleS)/sizeof(real_t);
	ifs.seekg(0, std::ios::beg);
	
	if (etPos == -1) etPos = numEle;
	if (stPos >= etPos || stPos < 0 || etPos > numEle)
	    throw std::runtime_error(std::string("Fail to read ")+dataPath);

	// read in the data
	data = Cpu::real_vector(etPos - stPos, 0);
	ifs.seekg(stPos * sizeof(real_t), std::ios::beg);
	for (long int i = 0; i<(etPos-stPos); i++){
	    ifs.read ((char *)&tempVal, sizeof(real_t));
	    data[i] = tempVal;
	}
	//thrust::copy(tempVec.begin(), tempVec.end(), data.begin());
	ifs.close();
	return (etPos - stPos);
    }

    int readRealDataAndFill(const std::string dataPath, Cpu::real_vector &buff, 
			    const int startPos, const int endPos, const int bufDim,
			    const int dataDim,  const int dataStartDim)
    {
	// 
	std::ifstream ifs(dataPath.c_str(), std::ifstream::binary | std::ifstream::in);
	if (!ifs.good())
	    throw std::runtime_error(std::string("Fail to open ")+dataPath);
	
	// get the number of data elements
	std::streampos numEleS, numEleE;
	long int numEle;
	long int stPos, etPos;
	real_t   tempVal;

	stPos = startPos; etPos = endPos;

	numEleS = ifs.tellg();
	ifs.seekg(0, std::ios::end);
	numEleE = ifs.tellg();
	numEle  = (numEleE-numEleS)/sizeof(real_t);
	ifs.seekg(0, std::ios::beg);
	
	if (etPos == -1) etPos = numEle;
	if (stPos >= etPos || stPos < 0 || etPos > numEle)
	    throw std::runtime_error(std::string("Fail to read in readReadlDataAndFill ")+dataPath);
 
	// read in the data
	long int timeIdx, dimIdx;
	ifs.seekg(stPos * sizeof(real_t), std::ios::beg);
	
	for (long int i = 0; i < (etPos-stPos); i++){
	    ifs.read ((char *)&tempVal, sizeof(real_t));
	    timeIdx = i / dataDim;
	    dimIdx  = i % dataDim;
	    buff[ timeIdx * bufDim + dataStartDim + dimIdx ] = tempVal;
	}
	//thrust::copy(tempVec.begin(), tempVec.end(), data.begin());
	ifs.close();
	return (etPos - stPos);
    }
    
    bool comp_seqs(const data_sets::DataSet::sequence_t &a, 
		   const data_sets::DataSet::sequence_t &b)
    {
        return (a.length < b.length);
    }

    struct rand_gen {
        unsigned operator()(unsigned i)
        {
            static boost::mt19937 *gen = NULL;
            if (!gen) {
                gen = new boost::mt19937;
                gen->seed(Configuration::instance().randomSeed());
            }

            boost::uniform_int<> dist(0, i-1);
            return dist(*gen);
        }
    };

} // namespace internal
} // anonymous namespace


namespace data_sets {

    struct thread_data_t
    {
        boost::thread             thread;
        boost::mutex              mutex;
        boost::condition_variable cv;
        bool                      terminate;
        
        boost::function<boost::shared_ptr<DataSetFraction> ()> taskFn;
        boost::shared_ptr<DataSetFraction> frac;
        bool finished;
    };

    void DataSet::_nextFracThreadFn()
    {
        for (;;) {
            // wait for a new task
            boost::unique_lock<boost::mutex> lock(m_threadData->mutex);
            while (m_threadData->taskFn.empty() && !m_threadData->terminate)
                m_threadData->cv.wait(lock);

            // terminate the thread?
            if (m_threadData->terminate)
                break;

            // execute the task
            m_threadData->frac.reset();
            m_threadData->frac = m_threadData->taskFn();
            m_threadData->finished = true;
            m_threadData->taskFn.clear();

            // tell the others that we are ready
            m_threadData->cv.notify_one();
        }
    }

    void DataSet::_shuffleSequences()
    {
        internal::rand_gen rg;
        std::random_shuffle(m_sequences.begin(), m_sequences.end(), rg);
    }

    void DataSet::_shuffleFractions()
    {
        std::vector<std::vector<sequence_t> > fractions;
        for (size_t i = 0; i < m_sequences.size(); ++i) {
            if (i % m_parallelSequences == 0)
                fractions.resize(fractions.size() + 1);
            fractions.back().push_back(m_sequences[i]);
        }

        internal::rand_gen rg;
        std::random_shuffle(fractions.begin(), fractions.end(), rg);

        m_sequences.clear();
        for (size_t i = 0; i < fractions.size(); ++i) {
            for (size_t j = 0; j < fractions[i].size(); ++j)
                m_sequences.push_back(fractions[i][j]);
        }
    }

    void DataSet::_addNoise(Cpu::real_vector *v)
    {
        if (!m_noiseDeviation)
            return;

        static boost::mt19937 *gen = NULL;
        if (!gen) {
            gen = new boost::mt19937;
            gen->seed(Configuration::instance().randomSeed());
        }

        boost::normal_distribution<real_t> dist((real_t)0, m_noiseDeviation);

        for (size_t i = 0; i < v->size(); ++i)
            (*v)[i] += dist(*gen);
    }

    Cpu::real_vector DataSet::_loadInputsFromCache(const sequence_t &seq)
    {
	/*
	if (0 && m_exInputFlag){
	    // read in external data case
	    Cpu::real_vector v(seq.length * m_exInputDim[0]);
	    m_cacheFile.seekg(seq.exInputBegin);
	    m_cacheFile.read((char*)v.data(), sizeof(real_t) * v.size());
	    assert (m_cacheFile.tellg() - seq.exInputBegin == v.size() * sizeof(real_t));
	    return v;
	}else{
	    // normal case
	    Cpu::real_vector v(seq.length * m_inputPatternSize);
	    m_cacheFile.seekg(seq.inputsBegin);
	    m_cacheFile.read((char*)v.data(), sizeof(real_t) * v.size());
	    assert (m_cacheFile.tellg() - seq.inputsBegin == v.size() * sizeof(real_t));
	    return v;
	    }*/
	Cpu::real_vector v(seq.length * m_inputPatternSize);
	m_cacheFile.seekg(seq.inputsBegin);
	m_cacheFile.read((char*)v.data(), sizeof(real_t) * v.size());
	assert (m_cacheFile.tellg() - seq.inputsBegin == v.size() * sizeof(real_t));
	return v;
    }

    Cpu::real_vector DataSet::_loadOutputsFromCache(const sequence_t &seq)
    {
        Cpu::real_vector v(seq.length * m_outputPatternSize);

        m_cacheFile.seekg(seq.targetsBegin);
        m_cacheFile.read((char*)v.data(), sizeof(real_t) * v.size());
        assert (m_cacheFile.tellg() - seq.targetsBegin == v.size() * sizeof(real_t));

        return v;
    }

    Cpu::real_vector DataSet::_loadExInputsFromCache(const sequence_t &seq)
    {
	Cpu::real_vector v(seq.exInputLength * seq.exInputDim);
	m_cacheFile.seekg(seq.exInputBegin);
	m_cacheFile.read((char*)v.data(), sizeof(real_t) * v.size());
	assert (m_cacheFile.tellg() - seq.exInputBegin == v.size() * sizeof(real_t));
	return v;
    }

    Cpu::real_vector DataSet::_loadExOutputsFromCache(const sequence_t &seq)
    {
	Cpu::real_vector v(seq.exOutputLength * seq.exOutputDim);
	m_cacheFile.seekg(seq.exOutputBegin);
	m_cacheFile.read((char*)v.data(), sizeof(real_t) * v.size());
	assert (m_cacheFile.tellg() - seq.exOutputBegin == v.size() * sizeof(real_t));
	return v;
    }

    Cpu::int_vector DataSet::_loadTargetClassesFromCache(const sequence_t &seq)
    {
        Cpu::int_vector v(seq.length);

        m_cacheFile.seekg(seq.targetsBegin);
        m_cacheFile.read((char*)v.data(), sizeof(int) * v.size());
        assert (m_cacheFile.tellg() - seq.targetsBegin == v.size() * sizeof(int));

        return v;
    }

    /*
    Cpu::real_vector DataSet::_loadTxtDataFromCache(const sequence_t &seq)
    {
        Cpu::real_vector v(seq.txtLength * m_txtDataPatternSize);

        m_cacheFile.seekg(seq.txtDataBegin);
        m_cacheFile.read((char*)v.data(), sizeof(int) * v.size());
        assert (m_cacheFile.tellg() - seq.txtDataBegin == v.size() * sizeof(int));

        return v;
	}*/

    Cpu::real_vector DataSet::_loadAuxRealDataFromCache(const sequence_t &seq)
    {
        Cpu::real_vector v(seq.length * m_auxDataDim);
        m_cacheFile.seekg(seq.auxDataBegin);
        m_cacheFile.read((char*)v.data(), sizeof(real_t) * v.size());
        assert (m_cacheFile.tellg() - seq.auxDataBegin == v.size() * sizeof(real_t));
        return v;
    }
    Cpu::pattype_vector DataSet::_loadAuxPattypeDataFromCache(const sequence_t &seq)
    {
        Cpu::pattype_vector v(seq.length * m_auxDataDim);
        m_cacheFile.seekg(seq.auxDataBegin);
        m_cacheFile.read((char*)v.data(), sizeof(char) * v.size());
        assert (m_cacheFile.tellg() - seq.auxDataBegin == v.size() * sizeof(char));
        return v;
    }
    Cpu::int_vector DataSet::_loadAuxIntDataFromCache(const sequence_t &seq)
    {
        Cpu::int_vector v(seq.length * m_auxDataDim);
        m_cacheFile.seekg(seq.auxDataBegin);
        m_cacheFile.read((char*)v.data(), sizeof(int) * v.size());
        assert (m_cacheFile.tellg() - seq.auxDataBegin == v.size() * sizeof(int));
        return v;
    }
    

    boost::shared_ptr<DataSetFraction> DataSet::_makeFractionTask(int firstSeqIdx)
    {
        int context_left   = Configuration::instance().inputLeftContext();
        int context_right  = Configuration::instance().inputRightContext();
        int context_length = context_left + context_right + 1;
        int output_lag     = Configuration::instance().outputTimeLag();

	Cpu::int_vector resolutionBuf;
	if (Configuration::instance().resolutions().size())
	    misFuncs::ParseIntOpt(Configuration::instance().resolutions(), resolutionBuf);
	else
	    resolutionBuf.clear();
	
	
        //printf("(%d) Making task firstSeqIdx=%d...\n", (int)m_sequences.size(), firstSeqIdx);
        boost::shared_ptr<DataSetFraction> frac(new DataSetFraction);

	frac->m_inputPatternSize  = m_inputPatternSize * context_length;
	
        frac->m_outputPatternSize = m_outputPatternSize;
        frac->m_maxSeqLength      = std::numeric_limits<int>::min();
        frac->m_minSeqLength      = std::numeric_limits<int>::max();

	// Add 0815: info of the external input data
	if (m_exInputFlag){
	    if (m_exInputDims.size())
		frac->m_exInputDim        = misFuncs::SumCpuIntVec(m_exInputDims);
	    else
		frac->m_exInputDim        = m_exInputDim;
	}

	if (m_exOutputFlag){
	    frac->m_exOutputDim        = misFuncs::SumCpuIntVec(m_exOutputDims);
	}

	frac->m_maxExInputLength  = std::numeric_limits<int>::min();
	frac->m_minExInputLength  = std::numeric_limits<int>::max();
	frac->m_maxExOutputLength  = std::numeric_limits<int>::min();
	frac->m_minExOutputLength  = std::numeric_limits<int>::max();
	
	// Dust #2017101205

        // fill fraction sequence info
        for (int seqIdx = firstSeqIdx; seqIdx < firstSeqIdx + m_parallelSequences; ++seqIdx) {
            if (seqIdx < (int)m_sequences.size()) {
                frac->m_maxSeqLength = std::max(frac->m_maxSeqLength, m_sequences[seqIdx].length);
                frac->m_minSeqLength = std::min(frac->m_minSeqLength, m_sequences[seqIdx].length);
		
		frac->m_maxExInputLength = std::max(frac->m_maxExInputLength,
						    m_sequences[seqIdx].exInputLength);
		frac->m_minExInputLength = std::min(frac->m_minExInputLength,
						    m_sequences[seqIdx].exInputLength);
		frac->m_maxExOutputLength = std::max(frac->m_maxExOutputLength,
						    m_sequences[seqIdx].exOutputLength);
		frac->m_minExOutputLength = std::min(frac->m_minExOutputLength,
						    m_sequences[seqIdx].exOutputLength);
		
                DataSetFraction::seq_info_t seqInfo;
                seqInfo.originalSeqIdx = m_sequences[seqIdx].originalSeqIdx;
                seqInfo.length         = m_sequences[seqIdx].length;
                seqInfo.seqTag         = m_sequences[seqIdx].seqTag;
		seqInfo.exInputLength  = m_sequences[seqIdx].exInputLength;
		seqInfo.exOutputLength = m_sequences[seqIdx].exOutputLength;
		
		// Dust #2017101206		
                frac->m_seqInfo.push_back(seqInfo);
            }
        }

        // allocate memory for the fraction
        frac->m_inputs.resize(frac->m_maxSeqLength * m_parallelSequences *
			      frac->m_inputPatternSize, 0);
        frac->m_patTypes.resize(frac->m_maxSeqLength * m_parallelSequences,
				PATTYPE_NONE);
	frac->m_fracTotalLength = 0;

	if (m_exInputFlag)
	    frac->m_exInputData.resize(frac->m_maxExInputLength * m_parallelSequences *
				       frac->m_exInputDim, 0);
	else
	    frac->m_exInputData.clear();

	if (m_exOutputFlag)
	    frac->m_exOutputData.resize(frac->m_maxExOutputLength * m_parallelSequences *
					frac->m_exOutputDim, 0);
	else
	    frac->m_exOutputData.clear();

	
	
	// prepare the resolution information buffer
	int patTypesResoLength = 0;
	for (int resoIdx = 0; resoIdx < resolutionBuf.size(); resoIdx++){
	    DataSetFraction::reso_info tempResoBuf;
	    tempResoBuf.resolution = resolutionBuf[resoIdx];
	    tempResoBuf.bufferPos  = patTypesResoLength;
	    tempResoBuf.length     = misFuncs::getResoLength(frac->m_patTypes.size(),
							     resolutionBuf[resoIdx]);
	    patTypesResoLength += tempResoBuf.length;
	    frac->m_resolutionBuffer.push_back(tempResoBuf);
	}
	frac->m_patTypesLowTimeRes.resize(patTypesResoLength, PATTYPE_NONE);
	
	// Dust #2017101207
	
	if (m_auxDirPath.size()>0){
	    if (m_auxDataTyp == AUXDATATYPE_CHAR){
		frac->m_auxPattypeData.resize(frac->m_maxSeqLength * m_auxDataDim *
					      m_parallelSequences, 0);
		frac->m_auxRealData.clear();
		frac->m_auxIntData.clear();
	    }else if(m_auxDataTyp == AUXDATATYPE_INT){
		frac->m_auxPattypeData.clear();
		frac->m_auxRealData.clear();
		frac->m_auxIntData.resize(frac->m_maxSeqLength * m_auxDataDim *
					  m_parallelSequences, 0);
	    }else if(m_auxDataTyp == AUXDATATYPE_FLOAT){
		frac->m_auxPattypeData.clear();
		frac->m_auxRealData.resize(frac->m_maxSeqLength * m_auxDataDim *
					   m_parallelSequences, 0.0);
		frac->m_auxIntData.clear();
	    }
	    frac->m_auxDataDim = m_auxDataDim;
	}else{
	    frac->m_auxPattypeData.clear();
	    frac->m_auxRealData.clear();
	    frac->m_auxIntData.clear();
	    frac->m_auxDataDim = -1;
	}

        if (m_isClassificationData)
            frac->m_targetClasses.resize(frac->m_maxSeqLength * m_parallelSequences, -1);
        else
            frac->m_outputs.resize((frac->m_maxSeqLength * m_parallelSequences *
				    m_outputPatternSize));


        // load sequences from the cache file and create the fraction vectors
        for (int i = 0; i < m_parallelSequences; ++i) {
	    
            if (firstSeqIdx + i >= (int)m_sequences.size())
                continue;

            const sequence_t &seq = m_sequences[firstSeqIdx + i];

            // load inputs data
            Cpu::real_vector inputs = _loadInputsFromCache(seq);
            _addNoise(&inputs);
	    //int tmpInputPatternSize = (m_exInputFlag)?(m_exInputDim[0]):(m_inputPatternSize);
            for (int timestep = 0; timestep < seq.length; ++timestep) {
                int srcStart = m_inputPatternSize * timestep;
                int offset_out = 0;
                for (int offset_in = -context_left; offset_in <= context_right; ++offset_in) {
                    int srcStart = m_inputPatternSize * (timestep + offset_in);
                    // duplicate first time step if needed
                    if (srcStart < 0) 
                        srcStart = 0;
                    // duplicate last time step if needed
                    else if (srcStart > m_inputPatternSize * (seq.length - 1))
                        srcStart = m_inputPatternSize * (seq.length - 1);
                    int tgtStart = frac->m_inputPatternSize * 
			(timestep * m_parallelSequences + i) +
			offset_out * m_inputPatternSize;
                    //std::cout << "copy from " << srcStart << " to " << tgtStart 
		    // << " size " << m_inputPatternSize << std::endl;
                    thrust::copy_n(inputs.begin() + srcStart, m_inputPatternSize, 
				   frac->m_inputs.begin() + tgtStart);
                    ++offset_out;
                }
            }
            /*std::cout << "original inputs: ";
            thrust::copy(inputs.begin(), inputs.end(), 
	    std::ostream_iterator<real_t>(std::cout, ";"));
            std::cout << std::endl;*/

            // target classes
            if (m_isClassificationData) {
                Cpu::int_vector targetClasses = _loadTargetClassesFromCache(seq);
                for (int timestep = 0; timestep < seq.length; ++timestep) {
                    int tgt = 0; // default class (make configurable?)
                    if (timestep >= output_lag)
                        tgt = targetClasses[timestep - output_lag];
                    frac->m_targetClasses[timestep * m_parallelSequences + i] = tgt;
                }
            }
	    
            // outputs
            else {
                Cpu::real_vector outputs = _loadOutputsFromCache(seq);
                for (int timestep = 0; timestep < seq.length; ++timestep) {
                    int tgtStart  = m_outputPatternSize * (timestep * m_parallelSequences + i);
                    if (timestep >= output_lag) {
                        int srcStart = m_outputPatternSize * (timestep - output_lag);
                        thrust::copy_n(outputs.begin() + srcStart, m_outputPatternSize, 
				       frac->m_outputs.begin() + tgtStart);
                    }else {
                        for (int oi = 0; oi < m_outputPatternSize; ++oi) {
                            frac->m_outputs[tgtStart + oi] = 1.0f; 
			    // default value (make configurable?)
                        }
                    }
                }
            }
	    
	    // Dust #2017101208
	    
	    if (m_exInputFlag){
		Cpu::real_vector exInput = _loadExInputsFromCache(seq);
		for (int timestep = 0; timestep < seq.exInputLength; ++timestep) {
		    int tgtStart  = seq.exInputDim * (timestep * m_parallelSequences + i);
		    int srcStart  = seq.exInputDim * timestep;
		    thrust::copy_n(exInput.begin() + srcStart, seq.exInputDim, 
				   frac->m_exInputData.begin() + tgtStart);
		}
	    }

	    if (m_exOutputFlag){
		Cpu::real_vector exOutput = _loadExOutputsFromCache(seq);
		for (int timestep = 0; timestep < seq.exOutputLength; ++timestep) {
		    int tgtStart  = seq.exOutputDim * (timestep * m_parallelSequences + i);
		    int srcStart  = seq.exOutputDim * timestep;
		    thrust::copy_n(exOutput.begin() + srcStart, seq.exOutputDim, 
				   frac->m_exOutputData.begin() + tgtStart);
		}
	    }

	    if (m_auxDirPath.size()>0){
		if (m_auxDataTyp == AUXDATATYPE_CHAR){
		    Cpu::pattype_vector auxData = _loadAuxPattypeDataFromCache(seq);
		    for (int timestep = 0; timestep < seq.length; ++timestep) {
			int tgtStart  = m_auxDataDim * (timestep * m_parallelSequences + i);
			if (timestep >= output_lag) {
			    int srcStart = m_auxDataDim * (timestep - output_lag);
			    thrust::copy_n(auxData.begin() + srcStart, m_auxDataDim, 
					   frac->m_auxPattypeData.begin() + tgtStart);
			}else {
			    for (int oi = 0; oi < m_auxDataDim; ++oi) 
				frac->m_auxPattypeData[tgtStart + oi] = 0;
			}
		    }
		    //thrust::copy(auxData.begin(), auxData.end(), frac->m_auxPattypeData.begin());
		}else if(m_auxDataTyp == AUXDATATYPE_INT){
		    Cpu::int_vector auxData = _loadAuxIntDataFromCache(seq);
		    for (int timestep = 0; timestep < seq.length; ++timestep) {
			int tgtStart  = m_auxDataDim * (timestep * m_parallelSequences + i);
			if (timestep >= output_lag) {
			    int srcStart = m_auxDataDim * (timestep - output_lag);
			    thrust::copy_n(auxData.begin() + srcStart, m_auxDataDim, 
					   frac->m_auxIntData.begin() + tgtStart);
			}else {
			    for (int oi = 0; oi < m_auxDataDim; ++oi) 
				frac->m_auxIntData[tgtStart + oi] = 0;
			}
		    }
		    //thrust::copy(auxData.begin(), auxData.end(), frac->m_auxIntData.begin());
		}else if(m_auxDataTyp == AUXDATATYPE_FLOAT){
		    Cpu::real_vector auxData = _loadAuxRealDataFromCache(seq);
		    for (int timestep = 0; timestep < seq.length; ++timestep) {
			int tgtStart  = m_auxDataDim * (timestep * m_parallelSequences + i);
			if (timestep >= output_lag) {
			    int srcStart = m_auxDataDim * (timestep - output_lag);
			    thrust::copy_n(auxData.begin() + srcStart, m_auxDataDim, 
					   frac->m_auxRealData.begin() + tgtStart);
			}else {
			    for (int oi = 0; oi < m_auxDataDim; ++oi) 
				frac->m_auxRealData[tgtStart + oi] = 1.0f;
			}
		    }
		    //thrust::copy(auxData.begin(), auxData.end(), frac->m_auxRealData.begin());
		}
	    }	

            // pattern types
            for (int timestep = 0; timestep < seq.length; ++timestep) {
                Cpu::pattype_vector::value_type patType;
                if (timestep == 0)
                    patType = PATTYPE_FIRST;
                else if (timestep == seq.length - 1)
                    patType = PATTYPE_LAST;
                else
                    patType = PATTYPE_NORMAL;

                frac->m_patTypes[timestep * m_parallelSequences + i] = patType;
		frac->m_fracTotalLength = frac->m_fracTotalLength+1;

		// also fill in the resolution buffer
		for (int resoIdx = 0; resoIdx < frac->m_resolutionBuffer.size(); resoIdx++){
		    int dataPos = timestep / frac->m_resolutionBuffer[resoIdx].resolution;
		    dataPos  = dataPos * m_parallelSequences + i;
		    dataPos += frac->m_resolutionBuffer[resoIdx].bufferPos;
		    frac->m_patTypesLowTimeRes[dataPos] =
			(frac->m_patTypesLowTimeRes[dataPos]==PATTYPE_FIRST?PATTYPE_FIRST:patType);
		}
            }
        }

        /*std::cout << "inputs for data fraction: ";
        thrust::copy(frac->m_inputs.begin(), frac->m_inputs.end(), 
	std::ostream_iterator<real_t>(std::cout, ";"));
        std::cout << std::endl; */
	
        return frac;
    }

    boost::shared_ptr<DataSetFraction> DataSet::_makeFirstFractionTask()
    {
        //printf("(%d) Making first task...\n", (int)m_sequences.size());
        
        if (m_sequenceShuffling)
            _shuffleSequences();
        if (m_fractionShuffling)
            _shuffleFractions();

        return _makeFractionTask(0);
    }

    DataSet::DataSet()
        : m_fractionShuffling(false)
        , m_sequenceShuffling(false)
        , m_noiseDeviation   (0)
        , m_parallelSequences(0)
        , m_totalSequences   (0)
        , m_totalTimesteps   (0)
        , m_minSeqLength     (0)
        , m_maxSeqLength     (0)
        , m_inputPatternSize (0)
        , m_outputPatternSize(0)
        , m_curFirstSeqIdx   (-1)
	, m_exInputFlag      (false)
	, m_exOutputFlag     (false)
	, m_auxDirPath       ("")
    {
    }

    DataSet::DataSet(const std::vector<std::string> &ncfiles, 
		     int parSeq, 
		     real_t fraction, int truncSeqLength, 
		     bool fracShuf,   bool seqShuf, 
		     real_t noiseDev, std::string cachePath
		     )
        : m_fractionShuffling(fracShuf)
        , m_sequenceShuffling(seqShuf)
        , m_noiseDeviation   (noiseDev)
        , m_parallelSequences(parSeq)
        , m_totalTimesteps   (0)
        , m_minSeqLength     (std::numeric_limits<int>::max())
        , m_maxSeqLength     (std::numeric_limits<int>::min())
        , m_curFirstSeqIdx   (-1)
    {
        int ret;
        int ncid;
	
        if (fraction <= 0 || fraction > 1)
            throw std::runtime_error("Invalid fraction");

	/* --- Preparation --- */
	// Add 1111: Prepare the auxillary data 
	static Configuration config = Configuration::instance();
	m_auxDirPath         = config.auxillaryDataDir();
	m_auxFileExt         = config.auxillaryDataExt();
	m_auxDataDim         = config.auxillaryDataDim();
	m_auxDataTyp         = config.auxillaryDataTyp();
	
	// Add 170327: Prepare the external input data
	if (config.exInputDir().size() || config.exInputDirs().size()){
	    if (config.exInputDim() > 0){
		// only single external input file
		m_exInputDir  = config.exInputDir();
		m_exInputExt  = config.exInputExt();
		m_exInputDim  = config.exInputDim();
		m_exInputFlag = true;
		m_exInputType = DATASET_EXINPUT_TYPE_1;
		m_exInputDirs.clear();
		m_exInputExts.clear();
		m_exInputDims.clear();
	    }else if (config.exInputDims().size() > 0){
		misFuncs::ParseStrOpt(config.exInputDirs(), m_exInputDirs, ",");
		misFuncs::ParseStrOpt(config.exInputExts(), m_exInputExts, ",");
		misFuncs::ParseIntOpt(config.exInputDims(), m_exInputDims);
		if (m_exInputDirs.size() != m_exInputExts.size() ||
		    m_exInputDirs.size() != m_exInputDims.size())
		    throw std::runtime_error("ExtInput options unequal length");
		m_exInputFlag = true;
		m_exInputType = DATASET_EXINPUT_TYPE_1;
		m_exInputDir  = "";
		m_exInputExt  = "";
		m_exInputDim  = 0;
	    }else{
		throw std::runtime_error("ExtInputDim(s) is not configured");
	    }
	}else{
	    m_exInputDir  = "";
	    m_exInputExt  = "";
	    m_exInputDim  = 0;
	    m_exInputFlag = false;
	    m_exInputType = DATASET_EXINPUT_TYPE_0;
	    m_exInputDirs.clear();
	    m_exInputExts.clear();
	    m_exInputDims.clear();
	}

	// Add 171012: Prepare the external output data
	if (config.exOutputDirs().size()){
	    misFuncs::ParseStrOpt(config.exOutputDirs(), m_exOutputDirs, ",");
	    misFuncs::ParseStrOpt(config.exOutputExts(), m_exOutputExts, ",");
	    misFuncs::ParseIntOpt(config.exOutputDims(), m_exOutputDims);
	    if (m_exOutputDirs.size() != m_exOutputExts.size() ||
		m_exOutputDirs.size() != m_exOutputDims.size())
		throw std::runtime_error("ExOutput options unequal length");
	    m_exOutputFlag = true;
	    m_exOutputType = DATASET_EXINPUT_TYPE_1;
	}else{
	    m_exOutputFlag = false;
	    m_exOutputType = DATASET_EXINPUT_TYPE_0;
	    m_exOutputDirs.clear();
	    m_exOutputExts.clear();
	    m_exOutputDims.clear();
	}
	
	
        // Preparation: cache data
        std::string tmpFileName = "";
        if (cachePath == "")
            tmpFileName = (boost::filesystem::temp_directory_path() / 
			   boost::filesystem::unique_path()).string();
        else
            tmpFileName = cachePath + "/" + (boost::filesystem::unique_path()).string();
        std::cerr << std::endl << "using cache file: " << tmpFileName << std::endl << "... ";
        m_cacheFileName = tmpFileName;
        m_cacheFile.open(tmpFileName.c_str(), 
			 std::fstream::in | std::fstream::out | 
			 std::fstream::binary | std::fstream::trunc);
        if (!m_cacheFile.good())
            throw std::runtime_error(std::string("Cannot open temporary file '") + 
				     tmpFileName + "'");

	/* --- Read in the data --- */
	
	// Read *.nc files
        bool first_file = true;
        for (std::vector<std::string>::const_iterator nc_itr = ncfiles.begin();
	     nc_itr != ncfiles.end(); ++nc_itr) 
        {
            std::vector<sequence_t> sequences;
            if ((ret = nc_open(nc_itr->c_str(), NC_NOWRITE, &ncid)))
                throw std::runtime_error(std::string("Could not open '") + 
					 *nc_itr + "': " + nc_strerror(ret));
            try {
                int maxSeqTagLength = internal::readNcDimension(ncid, "maxSeqTagLength");

		// Check input and output size
                if (first_file) {
                    m_isClassificationData = internal::hasNcDimension (ncid, "numLabels");
                    m_inputPatternSize     = internal::readNcDimension(ncid, "inputPattSize");
		    
                    if (m_isClassificationData) {
                        int numLabels       = internal::readNcDimension(ncid, "numLabels");
                        m_outputPatternSize = (numLabels == 2 ? 1 : numLabels);
                    }else{
                        m_outputPatternSize = internal::readNcDimension(ncid, "targetPattSize");
                    }
		    // Dust #2017101201
                }else{
		    if (m_isClassificationData) {
                        if (!internal::hasNcDimension(ncid, "numLabels")) 
                            throw std::runtime_error("Cannot classification with regression NC");
                        int numLabels = internal::readNcDimension(ncid, "numLabels");
                        if (m_outputPatternSize != (numLabels == 2 ? 1 : numLabels))
                            throw std::runtime_error("Number of classes mismatch in NC files");
                    }else{
                        if (m_outputPatternSize!= internal::readNcDimension(ncid,"targetPattSize"))
                            throw std::runtime_error("Number of targets mismatch in NC files");
                    }
                    if (m_inputPatternSize != internal::readNcDimension(ncid, "inputPattSize"))
                        throw std::runtime_error("Number of inputs mismatch in NC files");
		    // Dust #2017101202
                }

		// Read in sequence macro information
                int nSeq = internal::readNcDimension(ncid, "numSeqs");
                nSeq     = (int)((real_t)nSeq * fraction);
                nSeq     = std::max(nSeq, 1);
                int inputsBegin  = 0;
                int targetsBegin = 0;

                for (int i = 0; i < nSeq; ++i) {
                    int seqLength      = internal::readNcIntArray(ncid, "seqLengths", i);
                    m_totalTimesteps  += seqLength;
		    
		    // Dust #2017101203
                    std::string seqTag = internal::readNcStringArray(ncid, "seqTags", i, 
								     maxSeqTagLength);
                    int k = 0;
		    int rePosInUtt = 0;
                    while (seqLength > 0) {
                        sequence_t seq;
			// Fill in the information for seq
                        seq.originalSeqIdx = k;
                        if (truncSeqLength > 0 && seqLength > 1.5 * truncSeqLength) 
                            seq.length         = std::min(truncSeqLength, seqLength);
                        else
                            seq.length = seqLength;
                        seq.seqTag         = seqTag;
			//seq.txtLength    = txtLength;
			seq.beginInUtt     = rePosInUtt; 
                        sequences.push_back(seq);
                        seqLength         -= seq.length;
			rePosInUtt        += seq.length;
                        ++k;
			// Note: if this utterance is cut into several pieces, beginInUtt
			// logs the relative position of this piece in the utterance
                    }
                }

		// Read in sequence data
                for (std::vector<sequence_t>::iterator seq = sequences.begin(); 
		     seq != sequences.end(); ++seq) {
		    
                    m_minSeqLength = std::min(m_minSeqLength, seq->length);
                    m_maxSeqLength = std::max(m_maxSeqLength, seq->length);
		    //m_maxTxtLength = std::max(m_maxTxtLength, seq->txtLength);

                    // Step1. read input patterns and store them in the cache file
                    seq->inputsBegin = m_cacheFile.tellp();
                    Cpu::real_vector inputs =
			internal::readNcPatternArray(ncid, "inputs", inputsBegin, seq->length,
						     m_inputPatternSize);

		    // also prepare the external input data
		    if (m_exInputType == DATASET_EXINPUT_TYPE_1){
			if (m_inputPatternSize != 1)
			    throw std::runtime_error("input is not index for external input ");
			// When the input index is in increasing order
			// exInputStartPos and EndPos are used to load data from external files
			seq->exInputStartPos = inputs[0];   
			seq->exInputEndPos   = inputs[seq->length-1] + 1;

			// index is used to load the data in neural network
			// thus, the index should be shifted and starts from 0
			// Shift the index
			Cpu::real_vector tempVec(inputs.size(), inputs[0]);
			thrust::transform(inputs.begin(), inputs.end(), tempVec.begin(), 
					  inputs.begin(), thrust::minus<float>());
		    }else{
			seq->exInputStartPos = -1;
			seq->exInputEndPos   = -1;
		    }

                    m_cacheFile.write((const char*)inputs.data(), sizeof(real_t) * inputs.size());
                    assert (m_cacheFile.tellp() - seq->inputsBegin == 
			    seq->length * m_inputPatternSize * sizeof(real_t));

		    
                    // Step2. read targets and store them in the cache file
                    seq->targetsBegin = m_cacheFile.tellp();
                    if (m_isClassificationData) {
                        Cpu::int_vector targets = internal::readNcArray<int>(
							ncid, "targetClasses", targetsBegin,
							seq->length);
                        m_cacheFile.write((const char*)targets.data(), sizeof(int)*targets.size());
                        assert (m_cacheFile.tellp()-seq->targetsBegin==seq->length * sizeof(int));

			if (m_exOutputType == DATASET_EXINPUT_TYPE_1)
			    throw std::runtime_error("ExOutput not for the classification task");
			    
                    }else {
		

                        Cpu::real_vector targets = internal::readNcPatternArray(
							ncid, "targetPatterns", targetsBegin, 
							seq->length, m_outputPatternSize);

			// prepare the external output data
			if (m_exOutputType == DATASET_EXINPUT_TYPE_1){
			    if (m_outputPatternSize != 1)
				throw std::runtime_error("output is not index for ExOutput");
			    seq->exOutputStartPos = targets[0];   
			    seq->exOutputEndPos   = targets[seq->length-1] + 1;

			    Cpu::real_vector tempVec(targets.size(), targets[0]);
			    thrust::transform(targets.begin(), targets.end(), tempVec.begin(), 
					      targets.begin(), thrust::minus<float>());
			}else{
			    seq->exOutputStartPos = -1;
			    seq->exOutputEndPos   = -1;
			}
			
                        m_cacheFile.write((const char*)targets.data(),
					  sizeof(real_t) * targets.size());
                        assert (m_cacheFile.tellp() - seq->targetsBegin == 
				seq->length * m_outputPatternSize * sizeof(real_t));
                    }

		    
		    //Dust #2017101204
		    
		    // Step3. Add 1111: to read auxillary data from external binary data files
		    if (m_auxDirPath.size()>0){
			seq->auxDataBegin    = m_cacheFile.tellp();
			seq->auxDataDim      = m_auxDataDim;
			seq->auxDataTyp      = m_auxDataTyp;
			std::string fileName = m_auxDirPath + "/" + seq->seqTag + m_auxFileExt; 

			int dataShift  = seq->beginInUtt * seq->auxDataDim;
			int dataSize   = seq->length * seq->auxDataDim;    
			if (m_auxDataTyp == AUXDATATYPE_CHAR){
			    Cpu::pattype_vector temp;
			    int tempLength = internal::readCharData(fileName, temp);
			    if (tempLength < (dataShift + dataSize)){
				printf("Too short, auxillary data %s", fileName.c_str());
				throw std::runtime_error("Please check auxDataOption and data");
			    }
			    m_cacheFile.write((const char *)(temp.data() + dataShift),
					      sizeof(char) * dataSize);
			    assert(m_cacheFile.tellp()-seq->auxDataBegin==dataSize*sizeof(char));
			    
			}else if (m_auxDataTyp = AUXDATATYPE_INT){
			    Cpu::int_vector temp;
			    int tempLength = internal::readIntData(fileName, temp);
			    if (tempLength < (dataShift + dataSize)){
				printf("Too short, auxillary data %s", fileName.c_str());
				throw std::runtime_error("Please check auxDataOption and data");
			    }
			    m_cacheFile.write((const char *)(temp.data() + dataShift),
					      sizeof(int) * dataSize);
			    assert(m_cacheFile.tellp()-seq->auxDataBegin==dataSize*sizeof(int));
			}else if (m_auxDataTyp = AUXDATATYPE_FLOAT){
			    Cpu::real_vector temp;
			    int tempLength = internal::readRealData(fileName, temp, 0, -1);
			    if (tempLength < (dataShift + dataSize)){
				printf("To short, auxillary data %s", fileName.c_str());
				throw std::runtime_error("Please check auxDataOption and data");
			    }
			    m_cacheFile.write((const char *)(temp.data()+dataShift),
					      sizeof(real_t)* dataSize);
			    assert(m_cacheFile.tellp()-seq->auxDataBegin==dataSize*sizeof(real_t));
			}else{
			    throw std::runtime_error("Invalid auxDataTyp");
			}
		    }else{
			seq->auxDataBegin = 0;
			seq->auxDataDim   = 0;
			seq->auxDataTyp   = 0;
		    }

		    // Step4. Add 170327: to read external input data
		    if (m_exInputFlag){
			if (config.exInputDim() > 0){
			    // Only read one file among external input files
			    seq->exInputBegin    = m_cacheFile.tellp();
			    seq->exInputDim      = m_exInputDim;
			
			    std::string fileName = m_exInputDir+"/"+seq->seqTag+m_exInputExt; 
			    Cpu::real_vector temp;
			    int stPos, etPos;
			    if (m_exInputType == DATASET_EXINPUT_TYPE_1){
				stPos = seq->exInputStartPos * seq->exInputDim;
				etPos = seq->exInputEndPos   * seq->exInputDim;
			    }else{
				stPos = 0; etPos = -1;
			    }
			    int tempLength = internal::readRealData(fileName, temp, stPos, etPos);
			    seq->exInputLength   = tempLength / seq->exInputDim;
			    assert(seq->exInputLength * seq->exInputDim == tempLength);
			
			    m_cacheFile.write((const char *)(temp.data()),
					      sizeof(real_t) * tempLength);
			    assert((m_cacheFile.tellp()-seq->exInputBegin)==
				   (seq->exInputDim * seq->exInputLength * sizeof(real_t)));
			}else if (config.exInputDims().size() > 0){
			    // load multiple files
			    seq->exInputDim    = misFuncs::SumCpuIntVec(m_exInputDims);
			    seq->exInputLength = seq->exInputEndPos - seq->exInputStartPos;
			    Cpu::real_vector exDataBuf(seq->exInputDim *
						       (seq->exInputEndPos - seq->exInputStartPos),
						       0.0);
			    int cnt = 0;
			    int dimCnt = 0;
			    for (int i = 0; i < m_exInputDirs.size(); i++){
				std::string fileName = (m_exInputDirs[i] + "/" + seq->seqTag +
							m_exInputExts[i]); 

				int stPos, etPos;
				if (m_exInputType == DATASET_EXINPUT_TYPE_1){
				    stPos = seq->exInputStartPos * m_exInputDims[i];
				    etPos = seq->exInputEndPos   * m_exInputDims[i];
				}else{
				    stPos = 0; etPos = -1;
				}
				cnt += internal::readRealDataAndFill(
					fileName, exDataBuf, stPos, etPos,
					seq->exInputDim, m_exInputDims[i], dimCnt);
				dimCnt += m_exInputDims[i];
				
			    }
			    assert(seq->exInputLength * seq->exInputDim == cnt);
			    seq->exInputBegin    = m_cacheFile.tellp();
			    m_cacheFile.write((const char *)(exDataBuf.data()),
					      sizeof(real_t) * cnt);
			    assert((m_cacheFile.tellp()-seq->exInputBegin)==
				   (seq->exInputDim * seq->exInputLength * sizeof(real_t)));
			}else{
			    throw std::runtime_error("Impossible bug");
			}
		    }else{
			seq->exInputBegin  = 0;
			seq->exInputLength = 0;
			seq->exInputDim    = 0;
		    }

		    // Step5. To read external output data
		    if (m_exOutputFlag){
			// load multiple files
			seq->exOutputDim    = misFuncs::SumCpuIntVec(m_exOutputDims);
			seq->exOutputLength = seq->exOutputEndPos - seq->exOutputStartPos;
			Cpu::real_vector exDataBuf(seq->exOutputDim *
						   (seq->exOutputEndPos - seq->exOutputStartPos),
						   0.0);
			int cnt = 0;
			int dimCnt = 0;
			for (int i = 0; i < m_exOutputDirs.size(); i++){
			    std::string fileName = (m_exOutputDirs[i] + "/" + seq->seqTag +
						    m_exOutputExts[i]); 

			    int stPos, etPos;
				if (m_exOutputType == DATASET_EXINPUT_TYPE_1){
				    stPos = seq->exOutputStartPos * m_exOutputDims[i];
				    etPos = seq->exOutputEndPos   * m_exOutputDims[i];
				}else{
				    stPos = 0; etPos = -1;
				}
				cnt += internal::readRealDataAndFill(
					fileName, exDataBuf, stPos, etPos,
					seq->exOutputDim, m_exOutputDims[i], dimCnt);
				dimCnt += m_exOutputDims[i];
				
			    }
			    assert(seq->exOutputLength * seq->exOutputDim == cnt);
			    seq->exOutputBegin    = m_cacheFile.tellp();
			    m_cacheFile.write((const char *)(exDataBuf.data()),
					      sizeof(real_t) * cnt);
			    assert((m_cacheFile.tellp()-seq->exOutputBegin)==
				   (seq->exOutputDim * seq->exOutputLength * sizeof(real_t)));
			
		    }else{
			seq->exOutputBegin  = 0;
			seq->exOutputLength = 0;
			seq->exOutputDim    = 0;
		    }
		    
		    
                    inputsBegin  += seq->length; // position in data.nc
                    targetsBegin += seq->length; // position in data.nc
		    //txtDataBegin += seq->txtLength;
                }

                if (first_file) {
                    // retrieve output means + standard deviations, if they exist
                    try {
                        m_outputMeans  = internal::readNcArray<real_t>(
						   ncid, "outputMeans",  0, m_outputPatternSize);
                        m_outputStdevs = internal::readNcArray<real_t>(
                                                   ncid, "outputStdevs", 0, m_outputPatternSize);
                    }
                    catch (std::runtime_error& err) {
                        // Will result in "do nothing" when output unstandardization is used ...
                        m_outputMeans  = Cpu::real_vector(m_outputPatternSize, 0.0f);
                        m_outputStdevs = Cpu::real_vector(m_outputPatternSize, 1.0f);
                    }
                }

                // create next fraction data and start the thread
		// Note, every data.nc file will be handled by one thread
                m_threadData.reset(new thread_data_t);
                m_threadData->finished  = false;
                m_threadData->terminate = false;
                m_threadData->thread    = boost::thread(&DataSet::_nextFracThreadFn, this);
		nc_close(ncid);
            }
            catch (const std::exception&) {
                nc_close(ncid);
                throw;
            }
	    
            // append sequence structs from this nc file
            m_sequences.insert(m_sequences.end(), sequences.begin(), sequences.end());

            first_file = false;
        } // nc file loop

        m_totalSequences = m_sequences.size();
        // sort sequences by length
        if (Configuration::instance().trainingMode())
            std::sort(m_sequences.begin(), m_sequences.end(), internal::comp_seqs);
    }

    DataSet::~DataSet()
    {
        // terminate the next fraction thread
        if (m_threadData) {
            {{
                boost::lock_guard<boost::mutex> lock(m_threadData->mutex);
                m_threadData->terminate = true;
                m_threadData->cv.notify_one();
            }}

            m_threadData->thread.join();
        }
    }

    bool DataSet::isClassificationData() const
    {
        return m_isClassificationData;
    }

    bool DataSet::empty() const
    {
        return (m_totalTimesteps == 0);
    }

    boost::shared_ptr<DataSetFraction> DataSet::getNextFraction()
    {
        // initial work
        if (m_curFirstSeqIdx == -1) {
            boost::unique_lock<boost::mutex> lock(m_threadData->mutex);
            m_threadData->taskFn = boost::bind(&DataSet::_makeFirstFractionTask, this);
            m_threadData->finished = false;
            m_threadData->cv.notify_one();
            m_curFirstSeqIdx = 0;
        }

        // wait for the thread to finish
        boost::unique_lock<boost::mutex> lock(m_threadData->mutex);
        while (!m_threadData->finished)
            m_threadData->cv.wait(lock);

        // get the fraction
        boost::shared_ptr<DataSetFraction> frac;
        if (m_curFirstSeqIdx < (int)m_sequences.size()) {
            frac = m_threadData->frac;
            m_curFirstSeqIdx += m_parallelSequences;

            // start new task
            if (m_curFirstSeqIdx < (int)m_sequences.size())
                m_threadData->taskFn=boost::bind(&DataSet::_makeFractionTask,
						 this,m_curFirstSeqIdx);
            else
                m_threadData->taskFn=boost::bind(&DataSet::_makeFirstFractionTask,
						 this);

            m_threadData->finished = false;
            m_threadData->cv.notify_one();
        }
        else  {
            m_curFirstSeqIdx = 0;
        }

        return frac;
    }

    int DataSet::totalSequences() const
    {
        return m_totalSequences;
    }

    int DataSet::totalTimesteps() const
    {
        return m_totalTimesteps;
    }

    int DataSet::minSeqLength() const
    {
        return m_minSeqLength;
    }

    int DataSet::maxSeqLength() const
    {
        return m_maxSeqLength;
    }

    int DataSet::maxTxtLength() const
    {
	return 0;//m_maxTxtLength;
    }

    int DataSet::inputPatternSize() const
    {
	//if (m_exInputFlag)
	//    return m_exInputDim[0];
	//else
	return m_inputPatternSize;
    }

    int DataSet::outputPatternSize() const
    {
        return m_outputPatternSize;
    }

    Cpu::real_vector DataSet::outputMeans() const
    {
        return m_outputMeans;
    }

    Cpu::real_vector DataSet::outputStdevs() const
    {
        return m_outputStdevs;
    }

    std::string DataSet::cacheFileName() const
    {
        return m_cacheFileName;
    }

    
    // Add 0514 Wang: methods of DataSetMV
    /*
    	const int inputSize() const;
	const int outputSize() const;
	Cpu::real_vector& inputM();
	Cpu::real_vector& inputV();
	Cpu::real_vector& outputM();
	Cpu::real_vector& outputV();

	int    m_inputPatternSize;
        int    m_outputPatternSize;

        Cpu::real_vector m_inputMeans;
        Cpu::real_vector m_inputStdevs;

        Cpu::real_vector m_outputMeans;
        Cpu::real_vector m_outputStdevs;
	
	DataSetMV(const std::string &ncfile);
	~DataSetMV();
    */
    
    DataSetMV::DataSetMV(const std::string &ncfile)
	: m_inputPatternSize (-1)
	, m_outputPatternSize(-1)
    {
	int ret;
	int ncid;
	
	// read the mv data file
	if ((ret = nc_open(ncfile.c_str(), NC_NOWRITE, &ncid)))
	    throw std::runtime_error(std::string("Can't open mv file:")+ncfile);
	
	try{
	    // read the dimension
	    m_inputPatternSize  = internal::readNcDimension(ncid, "inputPattSize");
	    m_outputPatternSize = internal::readNcDimension(ncid, "targetPattSize");
	    
	    // read the mean and variance
	    m_inputMeans  = internal::readNcArray<real_t>(ncid,"inputMeans",  
							  0, m_inputPatternSize);
	    m_inputStdevs = internal::readNcArray<real_t>(ncid,"inputStdevs", 
							  0, m_inputPatternSize);
	    m_outputMeans = internal::readNcArray<real_t>(ncid,"outputMeans", 
							  0, m_outputPatternSize);
	    m_outputStdevs= internal::readNcArray<real_t>(ncid,"outputStdevs",
							  0, m_outputPatternSize);
	    
	    
	}catch (const std::exception&){
	    nc_close(ncid);
	    throw;
	}
	nc_close(ncid);
    }
    
    DataSetMV::DataSetMV()
	: m_inputPatternSize (-1)
	, m_outputPatternSize(-1)
    {
    }
    
    DataSetMV::~DataSetMV()
    {
    }
    
    const int& DataSetMV::inputSize() const
    {
	return m_inputPatternSize;
    }
    const int& DataSetMV::outputSize() const
    {
	return m_outputPatternSize;
    }

    const Cpu::real_vector& DataSetMV::inputM() const
    {
	return m_inputMeans;
    }
    const Cpu::real_vector& DataSetMV::inputV() const 
    {
	return m_inputStdevs;
    }
    const Cpu::real_vector& DataSetMV::outputM() const
    {
	return m_outputMeans;
    }
    const Cpu::real_vector& DataSetMV::outputV() const
    {
	return m_outputStdevs;
    }
    

} // namespace data_sets


/* Dust 

   Dust #2017101201
		    // Add Wang 0620: check the txt data
		    m_hasTxtData            = internal::hasNcDimension (ncid, "txtLength");
		    if (m_hasTxtData){
			m_txtDataPatternSize = internal::readNcDimension(ncid, "txtPattSize");
			if (m_txtDataPatternSize != 1)
			    throw std::runtime_error("Only support txtPattSize=1, in .nc files");
			if (truncSeqLength > 0){
			    printf("WARNING:truncSeqLength will be set to -1 in LstmCharW mode");
			    truncSeqLength = -1;
			}}

   Dust #2017101202
		    // Add Wang 0620: check the txt data
		    if (m_hasTxtData &&
			m_txtDataPatternSize != internal::readNcDimension(ncid, "txtPattSize")){
			throw std::runtime_error("txtPattSize mismatch");
			}

   Dust #2017101203		       
		    // Add Wang 0620: check the txt data
		    int txtLength      = (m_hasTxtData ?
					 (internal::readNcIntArray(ncid,"txtLengths",i)) : 0);
					 m_totalTxtLength  += txtLength;

   Dust #2017101204		       
		    // Add 0620 Wang: read and store the txt data
		    if (m_hasTxtData){
			seq->txtDataBegin       = m_cacheFile.tellp();
			Cpu::int_vector txtData = 
			    internal::readNcPatternArrayInt(ncid, "txtData", txtDataBegin, 
							    seq->txtLength,  m_txtDataPatternSize);
			
			m_cacheFile.write((const char *)txtData.data(),
					  sizeof(int) * txtData.size());
			
			assert (m_cacheFile.tellp() - seq->txtDataBegin == 
				seq->txtLength * m_txtDataPatternSize * sizeof(int));
		    }else{
			seq->txtDataBegin = 0;
		    }


   Dust #2017101205
	// Add 0620:  
	//frac->m_maxTxtLength      = std::numeric_limits<int>::min();
	//frac->m_txtPatternSize    = m_hasTxtData ? m_txtDataPatternSize : 0;

   Dust #2017101206
		frac->m_maxTxtLength   = m_hasTxtData ?
		    std::max(frac->m_maxTxtLength, m_sequences[seqIdx].txtLength) : 
		    frac->m_maxTxtLength;    
		    seqInfo.txtLength      = m_hasTxtData ? (m_sequences[seqIdx].txtLength) : 0;


   Dst #2017101207
	// Add 0620 Wang
	if (m_hasTxtData)
	    frac->m_txtData.resize((frac->m_maxTxtLength * m_parallelSequences* 
				    frac->m_txtPatternSize), 0);
	else
	frac->m_txtData.clear();


   Dust #2017101208
	// Add Wang 0620: Wang read in the txtData into fraction
	    if (m_hasTxtData){
		Cpu::int_vector txtData = _loadTxtDataFromCache(seq);
		for (int txtstep = 0; txtstep < seq.txtLength; txtstep++){
		     int txtStart = m_txtDataPatternSize * (txtstep * m_parallelSequences + i);
		     int srcStart = m_txtDataPatternSize * (txtstep);
		    thrust::copy_n(txtData.begin() + srcStart, m_txtDataPatternSize,
				   frac->m_txtData.begin() + txtStart);
		}
		}
	
*/

