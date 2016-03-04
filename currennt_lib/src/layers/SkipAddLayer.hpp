/****
 *
 *
 *
 ****/

#ifndef LAYERS_SKIPADDLAYER_HPP
#define LAYERS_SKIPADDLAYER_HPP

#include "TrainableLayer.hpp"
#include <boost/foreach.hpp>
#include <boost/shared_ptr.hpp>
#include <vector>

namespace layers {
    
    /**********************************************************************
	    Definition of the Skip Add layer
     

     **********************************************************************/
    
    // class definition
    template <typename TDevice>
    class SkipAddLayer : public TrainableLayer<TDevice>
    {
	typedef typename TDevice::real_vector    real_vector;
        
    private:
	// all the preceding skipping layers
	std::vector<Layer<TDevice>*> m_preLayers;
	// to receive the errors directly from next skip add layer
	real_vector       m_outputErrorsFromSkipLayer;
        
    public:
	
	
	// Construct the layer
	SkipAddLayer(
		     const helpers::JsonValue &layerChild,
		     const helpers::JsonValue &weightsSection,
		     std::vector<Layer<TDevice>*> precedingLayers
		     );

	// Destructor
	virtual ~SkipAddLayer();
	
	// void 
	virtual const std::string& type() const;

	// NN forward
	virtual void computeForwardPass();
	
	// NN backward
	virtual void computeBackwardPass();
	
	// return all the preceding layers
	std::vector<Layer<TDevice>*> PreLayers();
	
    };

}


#endif 


