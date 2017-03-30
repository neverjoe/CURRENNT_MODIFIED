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


#ifndef MACRODEFINE_HPP
#define MACRODEFINE_HPP


/*** For Feedback Model ***/

// 
#define NN_FEEDBACK_SCHEDULE_MIN 0.000 // Minimal value for the schedule sampling prob parameter

// Schedule sampling method code
#define NN_FEEDBACK_GROUND_TRUTH 0     // use ground truth directly
#define NN_FEEDBACK_DROPOUT_1N   1     // dropout, set to 1/N
#define NN_FEEDBACK_DROPOUT_ZERO 2     // dropout, set to zero
#define NN_FEEDBACK_SC_SOFT      3     // schedule sampling, use soft vector
#define NN_FEEDBACK_SC_MAXONEHOT 4     // schedule sampling, use one hot vector of the max prob

// Softmax generation method
#define NN_SOFTMAX_GEN_BEST      0
#define NN_SOFTMAX_GEN_SOFT      1
#define NN_SOFTMAX_GEN_SAMP      2


#endif