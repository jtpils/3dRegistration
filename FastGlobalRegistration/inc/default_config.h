#ifndef DFGCONFIG_H
#define DFGCONFIG_H

/* -------------------------------------------------------- */
/* Hard-coded macros (cannot be changed after compilation)  */

#define USE_OMP 	            //enables OpenMP
#define MIN_NUM_OF_CORR 10      // minimum number of correspondences required to compute the alignment
#define FLANN_LEAF_MAX_SIZE 30  // the maximum number of points to have in a leaf for not branching the tree any more
#define FLANN_SEARCH_CHECK 128   // specifies the maximum leafs to visit when searching for neighbours (-1 for UNLIMITED)

/* -------------------------------------------------------- */
/* The following can be changed on run time                 */

#define CLOSED_FORM			false
#define VERBOSE				false
#define USE_ABSOLUTE_SCALE	0
#define DIV_FACTOR			1.4
#define MAX_CORR_DIST		0.025
#define ITERATION_NUMBER	64
#define TUPLE_SCALE			0.95
#define TUPLE_MAX_CNT		1000
#define NORMALS_SEARCH_RAD	0.03
#define FPFH_SEARCH_RAD		0.2
#define STOP_RMSE			0.01

/* -------------------------------------------------------- */

#endif