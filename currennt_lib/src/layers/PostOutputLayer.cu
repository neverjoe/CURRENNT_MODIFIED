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

#include "PostOutputLayer.hpp"
#include "../helpers/getRawPointer.cuh"
#include "../MacroDefine.hpp"
#include <boost/lexical_cast.hpp>
#include <stdexcept>

#include <fstream>

namespace internal{
namespace {


    // Drop output the feedback data
    struct feedBackDropOut
    {
	real_t *dropProb;
	real_t *buffer;
	int bufDim;
	int parall;
	
	// from 1 to timesteps
        __host__ __device__ void operator() (const thrust::tuple<real_t&, const int&> &values) const
        {
	    const int outputIdx = values.get<1>() / bufDim;
	    const int dimIdx    = values.get<1>() % bufDim;
	    if ((dropProb && dropProb[(outputIdx/parall)] < 0.5) ||
		dropProb == NULL)
		buffer[outputIdx * bufDim + dimIdx] = 0.0;
        }
    };
    
}
}

namespace layers {

    template <typename TDevice>
    typename PostOutputLayer<TDevice>::real_vector& PostOutputLayer<TDevice>::_targets()
    {
        return this->outputs();
    }

    template <typename TDevice>
    typename PostOutputLayer<TDevice>::real_vector& PostOutputLayer<TDevice>::_actualOutputs()
    {
        return m_precedingLayer.outputs();
    }

    template <typename TDevice>
    typename PostOutputLayer<TDevice>::real_vector& PostOutputLayer<TDevice>::_outputErrors()
    {
        return m_precedingLayer.outputErrors();
    }
    
    /* Add 16-04-01 return the vector for mseWeight  */
    template <typename TDevice>
    typename PostOutputLayer<TDevice>::real_vector& PostOutputLayer<TDevice>::_mseWeight()
    {
	return m_outputMseWeights;
    }
    template <typename TDevice>
    typename PostOutputLayer<TDevice>::cpu_real_vector& PostOutputLayer<TDevice>::_mseWeightCPU()
    {
	return m_outputMseWeightsCPU;
    }
    
    template <typename TDevice>
    typename PostOutputLayer<TDevice>::real_vector& PostOutputLayer<TDevice>::_mvVector()
    {
	return m_targetDataMV;
    }
    
    template <typename TDevice>
    PostOutputLayer<TDevice>::PostOutputLayer(
        const helpers::JsonValue &layerChild, 
        Layer<TDevice> &precedingLayer,
        int requiredSize,
        bool createOutputs)
        : Layer<TDevice>  (layerChild, precedingLayer.parallelSequences(), 
			   precedingLayer.maxSeqLength(), createOutputs)
        , m_precedingLayer(precedingLayer)
    {
	// Modify 0506. For MDN, requireSize = -1, no need to check here
	// if (this->size() != requiredSize)
        if (requiredSize > 0 && this->size() != requiredSize){
	    printf("Layer size mis-match: %d vs %d",
		   this->size(), requiredSize);
	    throw std::runtime_error("Error in network.jsn");
	}
	
	/* Add 0401 wang */
	// assign the vector to output weights for RMSE
	// m_outputMseWeights = TDevice::real_vector(this->size(), (real_t)1.0);
	m_flagMseWeight = false;

	// Add 170411
	// Prepare the feedback data buffer
	m_feedBackOutput = this->outputs();
	
    }

    template <typename TDevice>
    PostOutputLayer<TDevice>::~PostOutputLayer()
    {
    }

    template <typename TDevice>
    void PostOutputLayer<TDevice>::loadSequences(const data_sets::DataSetFraction &fraction)
    {
        if (fraction.outputPatternSize() != this->size()) {
	    printf("Patter size mis match %d (layer) vs %d (data)",
		   this->size(), fraction.outputPatternSize());
            throw std::runtime_error("Error in network.jsn and data.nc");
        }

        Layer<TDevice>::loadSequences(fraction);

        if (!this->_outputs().empty())
        	thrust::copy(fraction.outputs().begin(), fraction.outputs().end(), 
			     this->_outputs().begin());
    }

    /* Add 0401 wang for weighted MSE*/
    // return flag
    template <typename TDevice>
    bool PostOutputLayer<TDevice>::flagMseWeight()
    {
	return this->m_flagMseWeight;
    }
    
    /* Add 1012 read the mean and variance vector */
    template <typename TDevice>
    bool PostOutputLayer<TDevice>::readMV(const PostOutputLayer<TDevice>::cpu_real_vector &mVec, 
					  const PostOutputLayer<TDevice>::cpu_real_vector &vVec)
    {
	if (mVec.size() != this->size() || vVec.size() != this->size()){
	    throw std::runtime_error(std::string("Unmatched number of dimension of mean and var"));
	}
	Cpu::real_vector tmpVec(this->size()*2, 0.0);
	thrust::copy(mVec.begin(), mVec.end(), tmpVec.begin());
	thrust::copy(vVec.begin(), vVec.end(), tmpVec.begin()+this->size());
	m_targetDataMV = tmpVec;
	return true;
    }

    
    // initialize the weight for Mse calculation
    template <typename TDevice>
    bool PostOutputLayer<TDevice>::readMseWeight(const std::string mseWeightPath)
    {
	std::ifstream ifs(mseWeightPath.c_str(), std::ifstream::binary | std::ifstream::in);
	if (!ifs.good()){
	    throw std::runtime_error(std::string("Fail to open ")+mseWeightPath);
	}
	m_flagMseWeight         = true;

	// get the number of we data
	std::streampos numEleS, numEleE;
	long int numEle;
	numEleS = ifs.tellg();
	ifs.seekg(0, std::ios::end);
	numEleE = ifs.tellg();
	numEle  = (numEleE-numEleS)/sizeof(real_t);
	ifs.seekg(0, std::ios::beg);
	
	if (numEle != this->size()){
	    printf("MSE weight vector length incompatible: %d %d", (int)numEle, (int)this->size());
	    throw std::runtime_error("Error in MSE weight configuration");
	}
	
	// read in the data
	real_t tempVal;
	std::vector<real_t> tempVec;
	for (unsigned int i = 0; i<numEle; i++){
	    ifs.read ((char *)&tempVal, sizeof(real_t));
	    tempVec.push_back(tempVal);
	}
	Cpu::real_vector tempVec2(numEle, 1.0);
	thrust::copy(tempVec.begin(), tempVec.end(), tempVec2.begin());
	m_outputMseWeights = tempVec2;
	m_outputMseWeightsCPU = tempVec2;
	
	std::cout << "Read #dim" << numEle << " mse vector" << std::endl;
	
	ifs.close();
	return true;
    }
    
    template <typename TDevice>
    void PostOutputLayer<TDevice>::reInitWeight()
    {
	// nothing to be done here
    }
    
    template <typename TDevice>
    Layer<TDevice>& PostOutputLayer<TDevice>::precedingLayer()
    {
        return m_precedingLayer;
    }    

    template <typename TDevice>
    bool PostOutputLayer<TDevice>::flagTrainable() const
    {
	return false;
    }

    // Functions to retrieve the feedback data
    template <typename TDevice>
    void PostOutputLayer<TDevice>::retrieveFeedBackData()
    {
	// directly copy the targets to the secondOutputs
	thrust::copy(this->outputs().begin(), this->outputs().end(),
		     m_feedBackOutput.begin());
    }
    
    template <typename TDevice>
    void PostOutputLayer<TDevice>::retrieveFeedBackData(real_vector& randNum, const int method)
    {
	// only used for dropout feedback (zero and 1/N backoff)
	internal::feedBackDropOut fn;
	fn.dropProb = helpers::getRawPointer(randNum);
	fn.buffer   = helpers::getRawPointer(m_feedBackOutput);
	fn.bufDim   = this->size();
	fn.parall   = this->m_precedingLayer.parallelSequences();

	int n = this->m_precedingLayer.curMaxSeqLength();
	n = n * this->m_precedingLayer.parallelSequences() * this->size();
	thrust::for_each(
	  thrust::make_zip_iterator(
		thrust::make_tuple(m_feedBackOutput.begin(), 
				   thrust::counting_iterator<int>(0))),
	  thrust::make_zip_iterator(
		thrust::make_tuple(m_feedBackOutput.begin() + n, 
				   thrust::counting_iterator<int>(0) + n)),
	  fn);
    }
    
    template <typename TDevice>
    void PostOutputLayer<TDevice>::retrieveFeedBackData(const int timeStep, const int method)
    {
	// only the generated output will be feedback in this function
	// In default, only one utterance is processed (parallel = 1)
	// Step1: copy 
	int startTime = timeStep * this->size();
	int endTime   = (timeStep + 1) * this->size();

	thrust::copy(this->precedingLayer().outputs().begin() + startTime,
		     this->precedingLayer().outputs().begin() + endTime,
		     m_feedBackOutput.begin()+ startTime);
	// Feed back Dropout
	if (method == NN_FEEDBACK_DROPOUT_1N || method == NN_FEEDBACK_DROPOUT_ZERO){
	    internal::feedBackDropOut fn;
	    fn.dropProb = NULL;
	    fn.buffer   = helpers::getRawPointer(m_feedBackOutput);
	    fn.bufDim   = this->size();
	    fn.parall   = this->m_precedingLayer.parallelSequences();
	    
	    thrust::for_each(
	      thrust::make_zip_iterator(
		thrust::make_tuple(m_feedBackOutput.begin() + startTime, 
				   thrust::counting_iterator<int>(0) + startTime)),
	      thrust::make_zip_iterator(
		thrust::make_tuple(m_feedBackOutput.begin() + endTime, 
				   thrust::counting_iterator<int>(0) + endTime)),
	      fn);
	}
    }
    
    template <typename TDevice>
    typename PostOutputLayer<TDevice>::real_vector& PostOutputLayer<TDevice>::secondOutputs(
		const bool flagTrain)
    {
	return m_feedBackOutput;
	/*if (flagTrain){
	    return this->outputs();
	}else{
	    return this->precedingLayer().outputs();
	}*/
    }
    
    template <typename TDevice>
    void PostOutputLayer<TDevice>::exportLayer(const helpers::JsonValue &layersArray, 
					      const helpers::JsonAllocator &allocator) const
    {
        Layer<TDevice>::exportLayer(layersArray, allocator);
    }

    // explicit template instantiations
    template class PostOutputLayer<Cpu>;
    template class PostOutputLayer<Gpu>;

} // namespace layers
