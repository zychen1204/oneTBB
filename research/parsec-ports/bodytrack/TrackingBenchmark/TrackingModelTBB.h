//-------------------------------------------------------------
//      ____                        _      _
//     / ___|____ _   _ ____   ____| |__  | |
//    | |   / ___| | | |  _  \/ ___|  _  \| |
//    | |___| |  | |_| | | | | |___| | | ||_|
//     \____|_|  \_____|_| |_|\____|_| |_|(_) Media benchmarks
//                  
//	  2006, Intel Corporation, licensed under Apache 2.0 
//
//  file : TrackingModelTBB.h
//  author : 
//  description : Observation model for kinematic tree body 
//				  tracking threaded with TBB.
//				  
//  modified : 
//--------------------------------------------------------------

#ifndef TRACKINGMODELTBB_H
#define TRACKINGMODELTBB_H

#if defined(HAVE_CONFIG_H)
# include "config.h"
#endif

#include "TrackingModel.h"
#include "tbb/global_control.h"
#include "tbb/parallel_for.h"
#include "tbb/parallel_reduce.h"
#include "tbb/blocked_range.h"
#include "TBBtypes.h"
#include <iostream>
#include <iomanip>
#include <sstream>

// oneTBB port: tbb::pipeline/filter were removed. The class keeps its stage
// function operator()(void*); main() now drives the stages in a plain loop,
// which is equivalent because the original used pipeline.run(1) (no overlap).
class TrackingModelTBB : public TrackingModel {

protected:
	
	unsigned int mCurFrame;			//current frame to process
	unsigned int mNumFrames;		//total frames to be processed

	//Generate an edge map from the original camera image - threaded
	virtual void CreateEdgeMap(FlexImage8u &src, FlexImage8u &dst);


public:
	TrackingModelTBB() : mCurFrame(0), mNumFrames(0) {};
	virtual ~TrackingModelTBB() {}

	//sets
	void SetNumFrames(unsigned int frames) {mNumFrames = frames; };

	//load and process images - overloaded for future threading
	bool GetObservation(float timeval);

	//give the model object the processed images
	void SetObservation(ImageSetToken &token) {mEdgeMaps = token.edgeMaps; mFGMaps = token.FGmaps; };

	//generate processed images for pipeline stage - these get passed to the next pipe stage defined by ParticleFilterTBB.h
	void *operator()(void *inToken);

};



#endif //TRACKINGMODELTBB_H
