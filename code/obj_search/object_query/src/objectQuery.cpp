#include "objectQuery.hpp"

namespace objsearch {
    namespace objectquery {
	ObjectQuery::ObjectQuery(int argc, char *argv[]){
	    ros::init(argc, argv, "object_query");
	    ros::NodeHandle handle;

	    // Retrieve the directory containing the cloud to be processed
	    ROSUtil::getParam(handle, "/object_query/query_features", queryFile_);
	    // Retrieve the directory containing the cloud of features generated by
	    // feature extraction
	    ROSUtil::getParam(handle, "/object_query/target_features", targetFile_);
	    ROSUtil::getParam(handle, "/object_query/output_regions", outputRegions_);

	    // Extract the K parameter specified in the launch file. If it is
	    // still at the default value of -1, read the parameter from the
	    // param file instead.
	    ROSUtil::getParam(handle, "/object_query/K", K_);
	    if (K_ == -1){
		ROSUtil::getParam(handle, "/obj_search/object_query/K", K_);
	    }

	    ROSUtil::getParam(handle, "/obj_search/processed_data_dir", dataPath_);
	    ROSUtil::getParam(handle, "/object_query/output_dir", outDir_);

	    ROSUtil::getParam(handle, "/object_query/x_step_hough", xStepHough_);
	    ROSUtil::getParam(handle, "/object_query/y_step_hough", yStepHough_);
	    ROSUtil::getParam(handle, "/object_query/z_step_hough", zStepHough_);

	    // If output is not specified, set the output directory to be the processed
	    // data directory specified by the global parameters.
	    if (std::string("NULL").compare(outDir_) == 0) {
		outDir_ = dataPath_;
	    }

	    // Query info is always the same, so initialise everything here.
	    // Stuff about the target will be initialised every loop. In the
	    // doSearch function
	    
	    // extract the remaining directories in the path of the file so that
	    // the data can be put into the output directory with the same path
	    // following it.
	    if (queryFile_.compare(0, dataPath_.size(), dataPath_) == 0){
		dataSubDir_ = sysutil::trimPath(std::string(queryFile_, dataPath_.size()), 1);
	    }
	    
	    // The output path for processed clouds is the subdirectory combined
	    // with the top level output directory. If dataSubDir_ is not
	    // initialised, then clouds are simply output to the top level
	    // output directory. The output will be written to a location for
	    // the query cloud, for each of the target clouds used.
	    outPath_ = sysutil::fullDirPath(sysutil::combinePaths(outDir_, dataSubDir_));
	    queryPointFile_ = sysutil::removeExtension(queryFile_, false) + "_points.pcd";
	    pcl::PCDReader reader;
	    pcl::PCLPointCloud2 queryHeader;
	    reader.readHeader(queryFile_, queryHeader);
	    queryType_ = queryHeader.fields[0].name;
	    
	    ROS_INFO("Loading query feature points from %s", queryPointFile_.c_str());

	    std::string match;
	    ROSUtil::getParam(handle, "/object_query/match", match);
	    // Get all paths to the target clouds to check. This depends on
	    // whether the parameter given is a file or directory, and also
	    // whether a match string was given or not.
	    if (sysutil::isDir(targetFile_)) {
		if (match.compare("NULL") == 0) {
		    targetClouds_ = sysutil::listFilesWithString(targetFile_, "nonPlanes.pcd", true);
		} else { // otherwise, match the string and process those files
		    targetClouds_ = sysutil::listFilesWithString(targetFile_, match, true);
		}
//		dataOutput = sysutil::fullDirPath(outPath_) + "featureparams_" + timeNow + ".yaml";
	    } else { // is a file, so just process that
//		dataOutput = sysutil::fullDirPath(outPath_) + "featureparams_" + timeNow + ".yaml";
		targetClouds_.push_back(targetFile_);
	    }

	    // print some information about what files will be processed
	    size_t i;
	    for (i = 0; i < 10 && i < targetClouds_.size(); i++) {
		ROS_INFO("%s", targetClouds_[i].c_str());
	    }
	    if (i >= 10) {
		ROS_INFO("And more...");
	    }

	    // Depending on the type of the descriptor in the cloud, we need to
	    // instantiate a different template for the search function
	    if (queryType_.find("shot") != std::string::npos) {
		// the only way to distinguish between colour shot and normal
		// shot is by checking the dimensionality of the descriptor
		int count = queryHeader.fields[0].count;
		if (count == 352) {
		    doSearch<pcl::SHOT352>();
		} else if (count == 1344) {
		    doSearch<pcl::SHOT1344>();
		} else {
		    ROS_ERROR("Unknown descriptor field specifier: %s", queryType_.c_str());
		    exit(1);
		}
	    } else if (queryType_.find("pfh") != std::string::npos) {
		if (queryType_.compare("pfh") == 0) {
		    doSearch<pcl::PFHSignature125>();
		} else if (queryType_.compare("fpfh") == 0) {
		    doSearch<pcl::FPFHSignature33>();
		} else if (queryType_.compare("pfhrgb") == 0) {
		    doSearch<pcl::PFHRGBSignature250>();
		} else {
		    ROS_ERROR("Unknown descriptor field specifier: %s", queryType_.c_str());
		    exit(1);
		}
	    } else if (queryType_.compare("shape_context") == 0) {
		doSearch<pcl::ShapeContext1980>();
	    } else {
		ROS_ERROR("Unknown descriptor field specifier: %s", queryType_.c_str());
                exit(1);
	    }
	}

	bool ObjectQuery::initAndCheckPaths(std::string path) {
	    targetFile_ = path;
	    // Read the headers for the point clouds that were provided as
	    // input, and look at the field names to determine which descriptor
	    // type is stored in the cloud.
	    pcl::PCDReader reader;
	    pcl::PCLPointCloud2 targetHeader;
	    reader.readHeader(targetFile_, targetHeader);
	    std::string targetType = targetHeader.fields[0].name;

	    // The descriptors for both clouds must be the same, otherwise we
	    // cannot compare them.
	    if (queryType_.compare(targetType) != 0){
		ROS_ERROR("Fields of the two descriptor clouds do not match: \n"\
			  "Query: %s, target: %s", queryType_.c_str(), targetType.c_str());
		return false;
	    }

	    // define the locations of the files containing the points at which
	    // features were extracted. Same as the file names, but with
	    // "_points" added on to the end
	    targetPointFile_ = sysutil::removeExtension(targetFile_, false) + "_points.pcd";

	    ROS_INFO("Loading target feature points from %s", targetPointFile_.c_str());

	    return true;
	}

	/** 
	 * Annotate points in the given cloud, using oriented bounding boxes
	 * computed from the annotated clouds.
	 *
	 * @param dir The top level room directory containing the cloud of
	 * interest (and more specifically the annotated clouds).
	 * @param cloud The cloud containing points to annotate.
	 * @param indices This vector will be populated with the indices of the
	 * points which have been labelled
	 * @param labels Will be populated with the labels of the points
	 */
	void ObjectQuery::annotatePointsOBB(
	    std::string dir, const pcl::PointCloud<pcl::PointXYZRGB>::Ptr& cloud,
	    std::vector<int>& indices, std::vector<std::string>& labels) {
	    // load all the annotated clouds and compute their bounding boxes.
	    // Don't care about rgb values
	    std::vector<pclutil::AnnotatedCloud<pcl::PointXYZ> > annotations
		= pclutil::getProcessedAnnotatedClouds<pcl::PointXYZ>(dir);

	    // fill a vector with the bounding boxes of each of the annotated clouds
	    std::vector<pclutil::OrientedBoundingBox> bboxes;
	    for (auto it = annotations.begin(); it != annotations.end(); it++) {
		bboxes.push_back(pclutil::getOrientedBoundingBox(it->cloud, it->label));
	    }

	    // go through each point in the cloud given and check if it lies in
	    // any of the bounding boxes of objects. If it lies in multiple
	    // boxes, the point will be added to the indices and labels arrays
	    // multiple times.
	    pcl::PointCloud<pcl::PointXYZRGB>::Ptr transformed(
		new pcl::PointCloud<pcl::PointXYZRGB>());
	    for (size_t i = 0; i < annotations.size(); i++) {
		std::cout << "Checking bbox for " << annotations[i].label << std::endl;
		// transform the points in the cloud into the frame of the box.
		pcl::transformPointCloud(*cloud, *transformed, bboxes[i].transformInverse);

		std::string currentLabel = annotations[i].label;
		for (size_t j = 0; j < transformed->size(); j++) {
		    if (bboxes[i].contains(transformed->points[j], true)){
			indices.push_back(j);
			labels.push_back(currentLabel);
		    }
		}
	    }
	}

	/** 
	 * Annotate points in a cloud loaded from a certain directory based on
	 * the annotations. Points in the given cloud will be compared to the
	 * annotated objects, and labelled with the label of the nearest object,
	 * but only if the point is within a euclidean distance of maxDist of
	 * the object. Uses nearest neighbour search for each point in the query
	 * cloud.
	 * 
	 * @param dir The top level room directory containing the cloud of
	 * interest.
	 * @param cloud The cloud containing points to annotate.
	 * @param indices This vector will be populated with the indices of the
	 * points which have been labelled
	 * @param labels Will be populated with the labels of the points
	 * @param distances Will be populated with the minimum distance of the point to its object
	 * @param maxDist The maximum distance from an object a point can be to
	 * still be considered part of the object
	 */
	void ObjectQuery::annotatePointsCloud(
	    std::string dir, const pcl::PointCloud<pcl::PointXYZRGB>::Ptr& cloud,
	    std::vector<int>& indices, std::vector<std::string>& labels,
	    std::vector<float>& distances, float maxDist) {

	    // extract the annotated clouds from the raw directory along with their labels
	    std::vector<pclutil::AnnotatedCloud<pcl::PointXYZRGB> > annotations
		= pclutil::getProcessedAnnotatedClouds<pcl::PointXYZRGB>(dir);

	    pcl::KdTreeFLANN<pcl::PointXYZRGB> searchTree;
	    // want to find the minimum distance and the corresponding index.

	    pcl::PointXYZRGB point;
	    int minInd = 0; // index of the point closest to the current object
	    float minDist = std::numeric_limits<float>::max(); // minimum distance from the point to an object
	    std::vector<int> nn(1); // index of nearest point on annotated object
	    std::vector<float> pointDistance(1); // distance of point from annotated object

	    // look through all the points in the cloud to be annotated
	    for (size_t i = 0; i < cloud->size(); i++) {

		point = cloud->points[i];

		// reset the min index and distance for the new point
		minInd = 0;
		minDist = std::numeric_limits<float>::max();
		// look through all the annotated object clouds and find the nearest
		// neighbour to the point received.
		for (size_t j = 0; j < annotations.size(); j++) {
		    searchTree.setInputCloud(annotations[j].cloud);
		    searchTree.nearestKSearch(point, 1, nn, pointDistance); // search for 1-nn
		    // update index and minimum distance to the object
		    if (pointDistance[0] < minDist){
			minInd = j;
			minDist = pointDistance[0];
		    }
		}

		// if the point is within the requested distance of the object,
		// push information about the point onto the vectors
		if (minDist < maxDist){
		    indices.push_back(i);
		    labels.push_back(annotations[minInd].label);
		    distances.push_back(minDist);
		    ROS_INFO("Point %d label %s", (int)i, labels.back().c_str());
		} else {
		    ROS_INFO("Point %d not labelled", (int)i);
		}
		
	    }
	}

	/** 
	 * Perform hough voting given the information provided. For each index
	 * in the indices given, which refer to points in the targetPoints
	 * cloud, we add a vote to a cell in 3d space. The votes should indicate
	 * regions where objects of interest lie
	 * 
	 * @param targetPoints The points to use to index into the 3d space
	 * which the grid defines
	 * @param indices The indices of the points at
	 * which we wish to place votes
	 * @param distances The distances to the points
	 */
	void ObjectQuery::houghVoting(const pcl::PointCloud<pcl::PointXYZRGB>::Ptr& targetPoints,
				      const std::vector<std::vector<int> >& indices,
				      const std::vector<std::vector<float> >& distances) {
	    // to construct the grid, need to know the bounds of the target
	    // points. Doesn't really matter if we waste some space on extra
	    // cells because the grid is not rotated (hopefully)
	    pcl::PointXYZRGB min;
	    pcl::PointXYZRGB max;
	    pcl::getMinMax3D(*targetPoints, min, max);
	    ROS_INFO("Grid dimensions %f, %f, %f", max.x - min.x, max.y - min.y, max.z - min.z);
	    pclutil::Grid3D grid(max.x - min.x, max.y - min.y, max.z - min.z,
				 xStepHough_, yStepHough_, zStepHough_,
				 min.x, min.y, min.z);

	    // Go through all points in the nearest neighbours
	    for (size_t i = 0; i < indices.size(); i++) {
		for (int j = 0; j < indices[i].size(); j++) {
		    pcl::PointXYZRGB cur = targetPoints->points[indices[i][j]];
		    grid.at(cur.x, cur.y, cur.z)++;
		}
	    }
	    int maxVal = *std::max_element(grid.values_.begin(), grid.values_.end());
	    std::vector<pcl::PointXYZ> centres = grid.allCentres();
	    pcl::PointCloud<pcl::PointXYZRGB>::Ptr voteCloud(new pcl::PointCloud<pcl::PointXYZRGB>());
	    ROS_INFO("Maximum votes in a single region %d", maxVal);
	    ROS_INFO("Total votes %d", std::accumulate(grid.values_.begin(), grid.values_.end(), 0));
	    ROS_INFO("Total cells %d", (int)grid.values_.size());
	    int empty = 0;
	    for (size_t i = 0; i < centres.size(); i++) {
		if (grid.at(i) == 0) {
		    empty++;
		    continue;
		}
		pcl::PointXYZRGB np;
		np.x = centres[i].x;
		np.y = centres[i].y;
		np.z = centres[i].z;
		pclutil::rgb colour = pclutil::getHeatColour(grid.at(i), maxVal);
		np.r = colour.r * 255;
		np.g = colour.g * 255;
		np.b = colour.b * 255;
		
		voteCloud->push_back(np);
	    }
	    ROS_INFO("Total empty cells %d", empty);

	    std::string out = std::string(sysutil::fullDirPath(outPath_) + "testhough.pcd");
	    ROS_INFO("Hough done, outputting to %s", out.c_str());

	    pcl::PCDWriter writer;
	    writer.write<pcl::PointXYZRGB>(out, *voteCloud, true);
	}

	/** 
	 * Do a nearest neighbour search for the features in the query cloud in
	 * the target cloud. 
	 * 
	 */
	template<typename DescType>
	void ObjectQuery::doSearch() {
	    ROS_INFO("Doing descriptor search.");
	    
	    pcl::PCDReader reader;

	    // Read the input clouds for the target and query descriptors. We
	    // want to find descriptors in targetFeatures which are close to
	    // those in queryFeatures. Need to use typename here because of
	    // dependent scope - what it is depends on the instantiation of the
	    // template argument. Each point in the *Points clouds corresponds
	    // to the location of the feature at the same index in the *Features
	    // clouds
	    typename pcl::PointCloud<DescType>::Ptr queryFeatures(new pcl::PointCloud<DescType>());
	    pcl::PointCloud<pcl::PointXYZRGB>::Ptr queryPoints(new pcl::PointCloud<pcl::PointXYZRGB>());
	    reader.read(queryPointFile_, *queryPoints);
	    reader.read(queryFile_, *queryFeatures);

	    std::vector<int> indicesQuery;
	    std::vector<std::string> labelsQuery;
	    // assume that the annotations are in a directory above the one in
	    // which feature clouds are stored.
	    annotatePointsOBB(sysutil::trimPath(queryPointFile_, 2), queryPoints, indicesQuery, labelsQuery);
	    ROS_INFO("query: %d annotated points of %d total", (int)indicesQuery.size(), (int)queryPoints->size());

	    // Create a flannsearch object to use to do the NN search
	    typename pcl::search::FlannSearch<DescType, flann::L2_Simple<float> >
		*search(new pcl::search::FlannSearch<DescType, flann::L2_Simple<float> >());
	    // Flann needs to know the point representation so that it can
	    // convert it to its internal format
	    typename pcl::DefaultPointRepresentation<DescType>::ConstPtr
		descRepr(new pcl::DefaultPointRepresentation<DescType>());
	    search->setPointRepresentation(descRepr);
	    
	    // loop over all clouds in the target cloud vector
	    for (size_t i = 0; i < targetClouds_.size(); i++) {
		// Do a check to make sure the feature types match. If not, skip
		// this target cloud
		if (!initAndCheckPaths(targetClouds_[i])) {
		    continue;
		}

		pcl::PointCloud<pcl::PointXYZRGB>::Ptr targetPoints(new pcl::PointCloud<pcl::PointXYZRGB>());
		typename pcl::PointCloud<DescType>::Ptr targetFeatures(new pcl::PointCloud<DescType>());
		targetPointFile_ = sysutil::removeExtension(targetClouds_[i], false) + "_points.pcd";
		reader.read(targetClouds_[i], *targetFeatures);
		reader.read(targetPointFile_, *targetPoints);
		search->setInputCloud(targetFeatures);
	    
		std::vector<int> indicesTarget;
		std::vector<std::string> labelsTarget;

		ROS_INFO("Starting search");
		// Loop over all points in the query cloud
		std::vector<std::vector<int> > nearest((int)queryFeatures->size());
		std::vector<std::vector<float> > square_dists((int)queryFeatures->size());
		// Initialise vectors to store the closest K points to the query point.	
		for (int i = 0; i < queryFeatures->size(); i++) {
		    ROS_INFO("Query point %d of %d", i + 1, (int)queryFeatures->size());
		    // some features may have nan values and will crash if not excluded.
		    if (!pclutil::isValid(queryFeatures->points[i])){
			continue;
		    }
		    // Search for the closest K points to the query point
		    search->nearestKSearch(queryFeatures->points[i], K_, nearest[i], square_dists[i]);
		}
		ROS_INFO("Finished finding neighbours");

		houghVoting(targetPoints, nearest, square_dists);

		exit(1);
		annotatePointsOBB(sysutil::trimPath(targetPointFile_, 2), targetPoints, indicesTarget, labelsTarget);
		ROS_INFO("target: %d annotated points of %d total", (int)indicesTarget.size(), (int)targetPoints->size());


		std::map<std::string, int> matches;
		// go through all the nearest neighbours of the query points which
		// have labels and see if there are matches
		for (size_t i = 0; i < indicesQuery.size(); i++) {
		    std::string currentQueryLabel = labelsQuery[i];
		    ROS_INFO("Checking matches for label %s at index %d",
			     currentQueryLabel.c_str(), indicesQuery[i]);
		    std::vector<int>& neigh = nearest[indicesQuery[i]];
		    ROS_INFO("Num neighbours %d", (int)neigh.size());
		    // go through the neighbours of this point
		    for (auto it = neigh.begin(); it != neigh.end(); it++) {
			ROS_INFO("Checking neighbour with target index %d", *it);
			// if the indices of labelled points in the target cloud
			// contain the neighbour we are looking at, and it has the
			// same label as the current point we are looking at in the
			// query cloud. 
			auto indit = std::find(indicesTarget.begin(),
					       indicesTarget.end(), *it);
			if (indit != indicesTarget.end()){
			    ROS_INFO("Found target index %d at location %d", (int)*it, (int)(indit - indicesTarget.begin()));
			    std::string targetLabel = labelsTarget[indit - indicesTarget.begin()];
			    ROS_INFO("Label is %s", targetLabel.c_str());
			    if (currentQueryLabel.compare(targetLabel) == 0){
				ROS_INFO("Labels match");
				// increment matches for the label
				matches[currentQueryLabel]++;
			    }
			}
		    }
		}

		for (auto it=matches.begin(); it!=matches.end(); ++it)
		    std::cout << it->first << " => " << it->second << '\n';

		ROS_INFO("Search complete");
	    }
	}

	// void outputRegions(){
	//     // if output regions is set, then we want to extract regions around the points at which features were computed
	//     pcl::PointCloud<pcl::PointXYZRGB>::Ptr targetPoints;
	//     pcl::PointCloud<pcl::PointXYZRGB>::Ptr queryPoints;
	//     if (outputRegions_){
	// 	// Use these to find the xyz position of the features in the clouds
	// 	targetPoints = new pcl::PointCloud<pcl::PointXYZRGB>();
	// 	queryPoints = new pcl::PointCloud<pcl::PointXYZRGB>();
	// 	reader.read(targetPointFile_, *targetPoints);
	// 	reader.read(queryPointFile_, *queryPoints);
	//     }

	    
	//     if (outputRegions_) {
	// 	pcl::PCDWriter writer;

	// 	// create kd trees to use to find points within a given radius of a
	// 	// specific point
	// 	pcl::KdTreeFLANN<pcl::PointXYZRGB> kdtreeQuery;
	// 	pcl::KdTreeFLANN<pcl::PointXYZRGB> kdtreeTarget;
	// 	kdtreeQuery.setInputCloud(queryPoints);
	// 	kdtreeTarget.setInputCloud(targetPoints);
	// 	std::vector<int> nn; // indices of points within the radius
	// 	std::vector<float> pointRadiusSquaredDistance; // distances of those points

	// 	float radius = 1;
	// 	// find points withing a given radius of the query point
	// 	kdtreeQuery.radiusSearch(queryPoints->points[0], radius, nn,
	// 				 pointRadiusSquaredDistance);
	    
	// 	ROS_INFO("%d points within radius %f of query point.", (int)nn.size(), radius);

	// 	// create a cloud to hold those points within the spherical region
	// 	pcl::PointCloud<pcl::PointXYZRGB>::Ptr regionPoints(new pcl::PointCloud<pcl::PointXYZRGB>());
	// 	for (int i = 0; i < nn.size(); i++) {
	// 	    regionPoints->push_back(queryPoints->points[nn[i]]);
	// 	}

	// 	// output the cloud
	// 	std::string filePath = sysutil::trimPath(queryFile_, 1);
	// 	std::string queryRegionOut = outPath_ + "queryRegion.pcd";
	// 	ROS_INFO("Outputting query point region to %s", queryRegionOut.c_str());
	// 	writer.write<pcl::PointXYZRGB>(queryRegionOut, *regionPoints, true);

	    
	// 	ROS_INFO("Query point (%f, %f, %f)", queryPoints->points[0].x,
	// 		 queryPoints->points[0].y, queryPoints->points[0].z);

	// 	// Loop over all the nearest neighbours and create regions for them as well.
	// 	for (int i = 0; i < K_; i++) {
	// 	    ROS_INFO("Index: %d, descriptor distance: %f", indices[i],
	// 		     square_dists[i]);
	// 	    ROS_INFO("Target point (%f, %f, %f)", targetPoints->points[indices[i]].x,
	// 		     queryPoints->points[indices[i]].y,
	// 		     queryPoints->points[indices[i]].z);
	// 	    // clear the spherical region to populate it with points in the
	// 	    // region of the target points
	// 	    regionPoints->clear();
	// 	    kdtreeTarget.radiusSearch(targetPoints->points[indices[i]],
	// 				      radius, nn, pointRadiusSquaredDistance);
	// 	    ROS_INFO("%d points within radius %f of matched target point.",
	// 		     (int)nn.size(), radius);
	// 	    // push all points from the radius search into the new cloud
	// 	    for (int j = 0; j < nn.size(); j++) {
	// 		regionPoints->push_back(targetPoints->points[nn[j]]);
	// 	    }

	// 	    // output the region cloud
	// 	    std::string targetRegionOut = outPath_ + "nn" + std::to_string(i) + ".pcd";
	// 	    ROS_INFO("Outputting target point region to %s", targetRegionOut.c_str());
	// 	    writer.write<pcl::PointXYZRGB>(targetRegionOut, *regionPoints, true);
	// 	}
	// }

    } // namespace objectquery
} // namespace obj_search

int main(int argc, char *argv[]) {
    objsearch::objectquery::ObjectQuery oq(argc, argv);
    
    return 0;
}
