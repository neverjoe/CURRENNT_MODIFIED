/******************************************************************************
 * This file is an addtional component of CURRENNT. 
 * Xin WANG
 * National Institute of Informatics, Japan
 * 2016
 *
 * This file is part of CURRENNT. 
 * Copyright (c) 2013 Johannes Bergmann, Felix Weninger, Bjoern Schuller
 * Institute for Human-Machine Communication
 * Technische Universitaet Muenchen (TUM)
 * D-80290 Munich, Germany
 *
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

#include "Amalgamate.hpp"

#include "../helpers/getRawPointer.cuh"
#include "../helpers/Matrix.hpp"
#include "../helpers/JsonClasses.hpp"
#include "../activation_functions/Logistic.cuh"
#include "../activation_functions/Tanh.cuh"

#include <thrust/transform.h>
#include <thrust/transform_reduce.h>
#include <thrust/for_each.h>
#include <thrust/iterator/constant_iterator.h>
#include <thrust/iterator/counting_iterator.h>
#include <thrust/fill.h>
#include <boost/foreach.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <vector>
#include <stdexcept>

#define AMALGAMATELAYER_DEBUG 0

namespace internal{
namespace {
    
    struct vectorFillOriginal
    {
	// Copy the output of preceding layer to the output of this layer
	// Copy the output of target layer to the output of this layer

	int dimInput1;      // dimension of output of preceding layer
	int dimOutput;      // dimension of output of this layer
	
	real_t *input;     // preceding layer
	real_t *output;     // this layer
	
	// dispatched over PreDim * T * Parallel
	__host__ __device__ void operator() (const thrust::tuple<const real_t&, int> &t)
	{
	    int inputIndex = t.get<1>();
	    int timeStep     = inputIndex / dimInput1;
	    int dimIdx       = inputIndex % dimInput1;
	    output[timeStep * dimOutput + dimIdx] = input[inputIndex];
	}
    };
    

    struct vectorAmalgamateForward
    {
	int dimOutput;      // dimension of output of this layer
	int dimPrelayer;
	
	int bandNum;
	
	real_t *input;      // target layer
	real_t *output;     // this layer

	char   *boundaryInfo;
	int     startTime;
	int     endTime;

	
	// dispatched over Dim * Band
	__host__ __device__ void operator() (const thrust::tuple<const real_t&, int> &t)
	{
	    int     dimIdxRel  = t.get<1>();              // relative index
	    int     outputIdx  = dimPrelayer + dimIdxRel; // position in output buffer
	    int     bandIdx    = dimIdxRel / dimPrelayer; // which band this ?
	    int     inputIdx   = dimIdxRel % dimPrelayer; // position in input buffer

	    
	    real_t  amalBuffer = 0.0;
	    real_t  amalState  = 0.0;
	    if (bandIdx    < bandNum){
		// forward direction
		outputIdx += startTime * dimOutput;
		inputIdx  += startTime * dimPrelayer;
		for (int time = startTime, step = 0; time < endTime; time++, step++){
		    if (boundaryInfo[time * bandNum * 2 + bandIdx] < 1 || time < 1){
			amalState  = amalBuffer;
			amalBuffer = 0.0;
			step       = 0;
		    }
		    output[outputIdx] = amalState;
		    // aggregating information using tanh and moving average
		    amalBuffer = ((step / (step + 1.0)) * amalBuffer +
				  input[inputIdx]  / (step + 1.0));
		    outputIdx += dimOutput;
		    inputIdx  += dimPrelayer;
		}
	    }else{
		outputIdx     += (endTime-1) * dimOutput;
		inputIdx      += (endTime-1) * dimPrelayer;
		// backward direction
		for (int time = (endTime-1), step = 0; time >= startTime; time--, step++){
		    if (boundaryInfo[time * bandNum * 2 + bandIdx] < 1 || time == (endTime-1)){
			amalState  = amalBuffer;
			amalBuffer = 0.0;
			step       = 0;
		    }
		    output[outputIdx]= amalState;
		    // aggregating information using tanh and moving average
		    amalBuffer  = ((step / (step+1.0)) * amalBuffer + input[inputIdx] / (step+1.0));
		    outputIdx -= dimOutput;
		    inputIdx  -= dimPrelayer;
		}
	    }
	}
    };


    struct vectorAmalgamateGradient
    {
	int dimOutput;      // dimension of output of this layer
	int dimPrelayer;
	int bandNum;
	
	real_t *gradients;  // gradients from the next layer
	
	char   *boundaryInfo;
	int     endTime;

	
	// dispatched over Dim * Band
	__host__ __device__ void operator() (const thrust::tuple<const real_t&, int> &t)
	{
	    int     dimIdxRel  = t.get<1>();              // relative index
	    int     outputIdx  = dimPrelayer + dimIdxRel; // position in output buffer
	    int     bandIdx    = dimIdxRel / dimPrelayer; // which band this ?
	    int     segLength  = 0;                       // length of the segment
	    
	    real_t  gradBuffer = 0.0;
	    real_t  gradState  = 0.0;
	    
	    if (bandIdx >= bandNum){
		// backward direction
		for (int time = 0; time < endTime; time++){
		    // position in both the gradient and the buffer
		    outputIdx   = time * dimOutput  + dimPrelayer + dimIdxRel;
		    gradBuffer += gradients[outputIdx];
		    gradients[outputIdx]= gradState * segLength;
		    
		    if (boundaryInfo[time * bandNum * 2 + bandIdx] < 1){
			gradState  = gradBuffer;
			gradBuffer = 0.0;
			if (time < (endTime-1)) // the segment length of this segment
			    segLength = boundaryInfo[(time+1)* bandNum * 2 + bandIdx];
		    }
		}

	    }else{
		// forward direction
		for (int time = (endTime-1); time >= 0; time--){
		    // position in both the gradient and the buffer
		    outputIdx   = time * dimOutput  + dimPrelayer + dimIdxRel;
		    gradBuffer += gradients[outputIdx];
		    gradients[outputIdx]= gradState * segLength;
		    
		    if (boundaryInfo[time * bandNum * 2 + bandIdx] < 1){
			gradState  = gradBuffer;
			gradBuffer = 0.0;
			if (time > 0) // the segment length of this segment
			    segLength = boundaryInfo[(time-1)* bandNum * 2 + bandIdx];
		    }
		}
	    }
	}
    };
    
    struct vectorAmalgamateBackward
    {
	int dimInput1;      // dimension of the preceding layer
	int dimOutput;      // dimension of this layer
	int bandNum;
	real_t *outputError;
	
	// dispatched over Dim * T * Parallel
	// Dim here is the dimension of the previous layer
	__host__ __device__ real_t operator() (const int &outputIdx) const
	{
	    int timeStep  = outputIdx / dimInput1;
	    int dimIdx    = outputIdx % dimInput1;
	    real_t temp   = 0.0;
	    for (int t = 0; t < bandNum; t++)
		temp += outputError[timeStep * dimOutput + dimIdx + t * dimInput1];
	    return temp;
	}
    };
    
}
}

namespace layers{

    void ParseAmalgamate(const std::string options, Cpu::int_vector &optVec){
	std::vector<std::string> tempArgs;
	boost::split(tempArgs, options, boost::is_any_of("_"));
	optVec.resize(tempArgs.size(), 0);
	for (int i =0 ; i<tempArgs.size(); i++)
	    optVec[i] = boost::lexical_cast<int>(tempArgs[i]);
    }

    void ConvertBoundaryInfoAmalgamate(Cpu::pattype_vector &boundary,
				       Cpu::pattype_vector &distance,
				       Cpu::int_vector & aggOpt,
				       const int curMaxLength)
    {
	std::vector<int> outTemp(aggOpt.size(), 0);
	// Amalgamate for the forward direction
	for (int time = 0; time < curMaxLength; time++){
	    for (int band = 0; band < aggOpt.size(); band++){
		if (boundary[time] & (0b01 << aggOpt[band]))
		    outTemp[band] = 0;
		else
		    outTemp[band] = outTemp[band] + 1;
		distance[time * aggOpt.size() * 2 + band] = outTemp[band];
	    }
	}
	
	// Amalgamate for the backward direction
	for (int time = curMaxLength-1; time >=0; time--){
	    for (int band = 0; band < aggOpt.size(); band++){
		// Note:
		// w1 w2 | w3 w4
		//       ^
		//       |
		//       boundary here notify the next time step (w3) is the start of 
		// the new segment. In backward conversion, w2 should be the start of
		// the new segment. Thus, it should look at the boundary[time+1]
		if (time == (curMaxLength-1)){
		    if (boundary[0] & (0b01 << aggOpt[band]))
			outTemp[band] = 0;
		    else
			outTemp[band] = outTemp[band] + 1;
		}else{
		    if (boundary[time+1] & (0b01 << aggOpt[band]))
			outTemp[band] = 0;
		    else
			outTemp[band] = outTemp[band] + 1;
		}
		distance[time * aggOpt.size() * 2 + band + aggOpt.size()] = outTemp[band];
	    }
	}
    }
    
    template <typename TDevice>
    AmalgamateLayer<TDevice>::AmalgamateLayer(const helpers::JsonValue &layerChild,
					  const helpers::JsonValue &weightsSection,
					  Layer<TDevice>           &precedingLayer
					  )
	: TrainableLayer<TDevice>(layerChild, weightsSection, 0, 0, precedingLayer)
    {

	// get aggregate information
	m_aggStr        = ((layerChild->HasMember("amalgamate")) ? 
			   ((*layerChild)["amalgamate"].GetString()) : (""));
	m_biDirectional = ((layerChild->HasMember("direction")) ? 
			   (((*layerChild)["direction"].GetString())[0] == 'b') : false);
	
	if (m_aggStr.size()){
	    // Parse the option
	    cpu_int_vector tempOpt;
	    ParseAmalgamate(m_aggStr, tempOpt);
	    m_aggOpt = tempOpt;
	    
	    // Buffer to the boundary information
	    // *2 is used to log down the distance in forward and backward direction
	    m_boundaryInfo.resize(m_aggOpt.size() * 2 * precedingLayer.maxSeqLength(), 0);
	}else{
	    throw std::runtime_error("Error in network.jsn: amalgamate is missing");
	}

	// dim * look_back + dim * aggregate + preceding_layer
	int dimExpected = (this->precedingLayer().size() * m_aggOpt.size()* (m_biDirectional?2:1) +
			   this->precedingLayer().size());
	
	if (dimExpected !=this->size()){
	    printf("Amalgamate layer size should be %d\n", dimExpected);
	    throw std::runtime_error("Error in network.jsn feedback layer size");
	}

	if (m_aggOpt.size())
	    m_aggBuffer.resize(this->precedingLayer().size() * m_aggOpt.size(), 0.0);
	
	// print information
	printf("\t[%s]\n", m_aggStr.c_str());
	
    }

    template <typename TDevice>
    AmalgamateLayer<TDevice>::~AmalgamateLayer()
    {
    }

    template <typename TDevice>
    void AmalgamateLayer<TDevice>::exportLayer(const helpers::JsonValue     &layersArray, 
					       const helpers::JsonAllocator &allocator) const
    {
        TrainableLayer<TDevice>::exportLayer(layersArray, allocator);
        (*layersArray)[layersArray->Size() - 1].AddMember("amalgamate", m_aggStr.c_str(),
							  allocator);
	static std::string s1;
	if (s1.empty()) s1 = "b";
	static std::string s2;
	if (s2.empty()) s2 = "u";
	if (m_biDirectional)
	    (*layersArray)[layersArray->Size() - 1].AddMember("direction", s1.c_str(),
							      allocator);
	else
	    (*layersArray)[layersArray->Size() - 1].AddMember("direction", s2.c_str(),
							      allocator);
    }

    template <typename TDevice>
    void AmalgamateLayer<TDevice>::loadSequences(const data_sets::DataSetFraction &fraction)
    {
	TrainableLayer<TDevice>::loadSequences(fraction);

	if (fraction.auxDataDim()>0){
	    // Load the boundary information for amalgamation
	    if (m_aggStr.size() < 1)
		throw std::runtime_error("Aggregate information is not specified");
	    if (m_aggOpt.size() > CHAR_BIT)
		throw std::runtime_error("Aggregate information is larger than CHAR_BIT");

	    // Read in the aux label information
	    Cpu::pattype_vector auxInfo = fraction.auxPattypeData();
	    if (auxInfo.size() != this->curMaxSeqLength())
		throw std::runtime_error("Error unequal length of clockTime size");
	    
	    // Convert the boundary information into distance information
	    Cpu::pattype_vector tempDistance(m_boundaryInfo.size(), 0);
	    cpu_int_vector      tmpAggOpt = m_aggOpt;
	    
	    ConvertBoundaryInfoAmalgamate(auxInfo, tempDistance, tmpAggOpt,
					  this->curMaxSeqLength());
	    
	    m_boundaryInfo = tempDistance;
	    
	    if (AMALGAMATELAYER_DEBUG){
		for (int i = 0; i < this->curMaxSeqLength(); i++){
		    printf("%d:%3d\t", i, auxInfo[i]);
		    for (int j = 0; j<m_aggOpt.size()*2; j++)
			printf("%3d ", tempDistance[i*m_aggOpt.size()*2+j]);
		    printf("\n");
		}
	    }

	    // prepare the aggregate buffer (which will be used in generation)
	    m_aggBuffer.resize(this->precedingLayer().size() * m_aggOpt.size(), 0.0);
	    
	}else{
	    // Just use fixed step amalgamation
	    printf("Amalgamation for fixed interval is not implemented\n");
	    throw std::runtime_error("Boundary information must be provided as auxData");
	}	
    }
    
    template <typename TDevice>
    const std::string& AmalgamateLayer<TDevice>::type() const
    {
        static std::string s;
        if (s.empty()) s = "amalgamate";
        return s;
    }

    // computeForward: 
    //  in training stage, target data are known
    template <typename TDevice>
    void AmalgamateLayer<TDevice>::computeForwardPass()
    {
	
	thrust::fill(this->outputs().begin(), this->outputs().end(), 0.0);
	{{
	    internal::vectorFillOriginal fn;
	    fn.dimInput1      = this->precedingLayer().size();	
	    fn.dimOutput      = this->size();     
	    fn.input          = helpers::getRawPointer(this->precedingLayer().outputs());
	    fn.output         = helpers::getRawPointer(this->outputs());
	    
	    int n = this->curMaxSeqLength()*this->parallelSequences()*this->precedingLayer().size();
	    thrust::for_each(
		thrust::make_zip_iterator(thrust::make_tuple(this->outputs().begin(),
							     thrust::counting_iterator<int>(0))),
		thrust::make_zip_iterator(thrust::make_tuple(this->outputs().begin()+n,
							     thrust::counting_iterator<int>(0)+n)),
		fn);
	}}
	
	{{

	    internal::vectorAmalgamateForward fn;
	    fn.dimOutput      = this->size();
	    fn.dimPrelayer    = this->precedingLayer().size();
	    fn.bandNum        = this->m_aggOpt.size();
	    
	    fn.input          = helpers::getRawPointer(this->precedingLayer().outputs());
	    fn.output         = helpers::getRawPointer(this->outputs());
	    
	    fn.boundaryInfo   = helpers::getRawPointer(this->m_boundaryInfo);
	    fn.startTime      = 0;
	    fn.endTime        = this->curMaxSeqLength();
	    	
	    int n = this->precedingLayer().size() * m_aggOpt.size() * (m_biDirectional?2:1);
	    thrust::for_each(
		 thrust::make_zip_iterator(
				thrust::make_tuple(this->outputs().begin(),
						   thrust::counting_iterator<int>(0))),
		 thrust::make_zip_iterator(
				thrust::make_tuple(this->outputs().begin()+n,
						   thrust::counting_iterator<int>(0)+n)),
		fn);
	}}
    }

    // computeForwardPass
    // in synthesis stage, when the target must be predicted frame by frame
    template <typename TDevice>
    void AmalgamateLayer<TDevice>::computeForwardPass(const int timeStep)
    {
	// to be implemented
    }

    // 
    template <typename TDevice>
    void AmalgamateLayer<TDevice>::computeBackwardPass()
    {
	
	{{
	   internal::vectorAmalgamateGradient fn;
	   fn.dimOutput      = this->size();
	   fn.dimPrelayer    = this->precedingLayer().size();
	   fn.bandNum        = this->m_aggOpt.size();
	   fn.gradients      = helpers::getRawPointer(this->outputErrors());
	   fn.boundaryInfo   = helpers::getRawPointer(this->m_boundaryInfo);
	   fn.endTime        = this->curMaxSeqLength();
	   int n = this->precedingLayer().size() * m_aggOpt.size() * (m_biDirectional?2:1);
	   thrust::for_each(
		 thrust::make_zip_iterator(
				thrust::make_tuple(this->outputs().begin(),
						   thrust::counting_iterator<int>(0))),
		 thrust::make_zip_iterator(
				thrust::make_tuple(this->outputs().begin()+n,
						   thrust::counting_iterator<int>(0)+n)),
		fn);

	}}
	
	{{
	   // Copy the gradient for the preceding layer
	   internal::vectorAmalgamateBackward fn;
	   fn.dimInput1      = this->precedingLayer().size();
	   fn.dimOutput      = this->size();
	   fn.outputError    = helpers::getRawPointer(this->outputErrors());
	   fn.bandNum        = m_aggOpt.size() * (m_biDirectional?2:1);
	   int n = (this->curMaxSeqLength() * this->parallelSequences() *
		    this->precedingLayer().size());
	   
	   thrust::transform(thrust::counting_iterator<int>(0),
			     thrust::counting_iterator<int>(0)+n,
			     this->precedingLayer().outputErrors().begin(),
			     fn);	   
	}}
    }

    template <typename TDevice>
    void AmalgamateLayer<TDevice>::setDirection(const bool direction)
    {
	m_biDirectional = direction;
    }
    
    template class AmalgamateLayer<Cpu>;
    template class AmalgamateLayer<Gpu>;
    
}
