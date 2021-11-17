/*
 * frontier_detector_dms.cpp
 *
 *  Created on: Sep 25, 2021
 *      Author: hankm
 */

// frontier detection for dynamic map size cases (cartographer generated maps)

#include "frontier_detector_dms.hpp"


namespace autoexplorer
{

FrontierDetectorDMS::FrontierDetectorDMS(const ros::NodeHandle private_nh_, const ros::NodeHandle &nh_):
m_nh_private(private_nh_),
m_nh(nh_),
m_nglobalcostmapidx(0),
m_isInitMotionCompleted(false),
mpo_gph(NULL)
{
	// gridmap generated from octomap might be downsampled !!

	float fcostmap_conf_thr, fgridmap_conf_thr ;
	m_nh.getParam("/autoexplorer/debug_data_save_path", m_str_debugpath);
	m_nh.param("/autoexplorer/costmap_conf_thr", fcostmap_conf_thr, 0.1f);
	m_nh.param("/autoexplorer/gridmap_conf_thr", fgridmap_conf_thr, 0.8f);
	m_nh.param("/autoexplorer/occupancy_thr", m_noccupancy_thr, 50);
	m_nh.param("/autoexplorer/lethal_cost_thr", m_nlethal_cost_thr, 80);
	m_nh.param("/autoexplorer/global_width", m_nGlobalMapWidth, 4000) ;
	m_nh.param("/autoexplorer/global_height", m_nGlobalMapHeight, 4000) ;
	m_nh.param("/move_base_node/global_costmap/resolution", m_fResolution, 0.05f) ;

//	int _nWeakCompThreshold	= m_fs["WEAK_COMP_THR"];
//	m_frontiers_region_thr = _nWeakCompThreshold / m_nScale ;
	int _nWeakCompThreshold ;
	m_nh.param("/autoexplorer/weak_comp_thr", _nWeakCompThreshold, 10);
	m_nh.param("/autoexplorer/num_downsamples", m_nNumPyrDownSample, 0);
	m_nh.param("/autoexplorer/frame_id", m_worldFrameId, std::string("map"));
	m_nh.param("move_base_node/global_costmap/robot_radius", m_fRobotRadius, 0.3);

	m_nScale = pow(2, m_nNumPyrDownSample) ;
	m_frontiers_region_thr = _nWeakCompThreshold / m_nScale ;
	m_nROISize = static_cast<int>( round( m_fRobotRadius / m_fResolution ) ) * 2 ; // we never downsample costmap !!! dont scale it with roisize !!

	m_nGlobalMapCentX = m_nGlobalMapWidth  / 2 ;
	m_nGlobalMapCentY = m_nGlobalMapHeight / 2 ;

	m_targetspub = m_nh.advertise<geometry_msgs::PointStamped>("detected_points", 10);
	m_currentgoalpub = m_nh.advertise<geometry_msgs::PoseWithCovarianceStamped>("curr_goalpose", 10);
	m_makergoalpub = m_nh.advertise<visualization_msgs::Marker>("curr_goal_shape",10);
	m_markercandpub = m_nh.advertise<visualization_msgs::Marker>("detected_shapes", 10);
	m_markerfrontierpub = m_nh.advertise<visualization_msgs::Marker>("filtered_shapes", 10);
	m_unreachpointpub = m_nh.advertise<visualization_msgs::Marker>("unreachable_shapes", 10);

	m_velpub		= m_nh.advertise<geometry_msgs::Twist>("cmd_vel",10);
	m_donepub		= m_nh.advertise<std_msgs::Bool>("move_base_simple/mapping_is_done",1);

	//---------------------------------------------------------------
	//m_mapsub = m_nh.subscribe("map", 1, &FrontierDetectorDMS::gridmapCallBack, this);  // "projected_map" if octomap is on
	m_mapframedataSub  	= m_nh.subscribe("map", 1, &FrontierDetectorDMS::mapdataCallback, this); // kmHan
	//m_globalplanSub 	= m_nh.subscribe("move_base_node/NavfnROS/plan",1 , &FrontierDetectorDMS::moveRobotCallback, this) ; // kmHan
	m_globalplanSub 	= m_nh.subscribe("curr_goalpose",1 , &FrontierDetectorDMS::moveRobotCallback, this) ; // kmHan
	m_globalCostmapSub 	= m_nh.subscribe("move_base_node/global_costmap/costmap", 1, &FrontierDetectorDMS::globalCostmapCallBack, this );
//	m_globalCostmapUpdateSub
//				= m_nh.subscribe("move_base_node/global_costmap/costmap_updates", 1, &FrontierDetectorDMS::globalCostmapUpdateCallback, this );

	m_poseSub		   	= m_nh.subscribe("pose", 10, &FrontierDetectorDMS::robotPoseCallBack, this);
	m_velSub			= m_nh.subscribe("cmd_vel", 10, &FrontierDetectorDMS::robotVelCallBack, this);
	m_unreachablefrontierSub = m_nh.subscribe("unreachable_frontier", 1, &FrontierDetectorDMS::unreachablefrontierCallback, this);
	m_makeplan_client = m_nh.serviceClient<nav_msgs::GetPlan>("move_base_node/make_plan");
ROS_WARN("allocating map buff \n");
ROS_WARN("nscale: %d \n", m_nScale);

	m_uMapImg  	  = cv::Mat(m_nGlobalMapHeight, m_nGlobalMapWidth, CV_8U, cv::Scalar(127));

	int ncostmap_roi_size = m_nROISize / 2 ;
	int ngridmap_roi_size = m_nROISize ;
	m_nCorrectionWindowWidth = m_nScale * 2 + 1 ; // the size of the correction search window

	m_oFrontierFilter = FrontierFilter(
			ncostmap_roi_size, ngridmap_roi_size, m_str_debugpath, m_nNumPyrDownSample,
			fgridmap_conf_thr, fcostmap_conf_thr, m_noccupancy_thr, m_nlethal_cost_thr,
			m_nGlobalMapWidth, m_nGlobalMapHeight,
			m_fResolution);
	while(!m_move_client.waitForServer(ros::Duration(5.0)))
	//while(!m_move_client.waitForActionServerToStart())
	{
		ROS_INFO("Waiting for the move_base action server to come up");
	}
ROS_WARN("move_base action server is up");


// global_planning_handler
	//mpo_gph = new GlobalPlanningHandler( ) ;
	mpo_costmap = new costmap_2d::Costmap2D();

	if (mp_cost_translation_table == NULL)
	{
		mp_cost_translation_table = new uint8_t[101];

		// special values:
		mp_cost_translation_table[0] = 0;  // NO obstacle
		mp_cost_translation_table[99] = 253;  // INSCRIBED obstacle
		mp_cost_translation_table[100] = 254;  // LETHAL obstacle
//		mp_cost_translation_table[-1] = 255;  // UNKNOWN

		// regular cost values scale the range 1 to 252 (inclusive) to fit
		// into 1 to 98 (inclusive).
		for (int i = 1; i < 99; i++)
		{
			mp_cost_translation_table[ i ] = uint8_t( ((i-1)*251 -1 )/97+1 );
		}
	}

	// Set markers

	SetVizMarkers( m_worldFrameId, 1.f, 0.f, 0.f, 0.2, m_cands );
	SetVizMarkers( m_worldFrameId, 0.f, 1.f, 0.f, 0.3, m_points );
	SetVizMarkers( m_worldFrameId, 1.f, 0.f, 1.f, 0.5, m_exploration_goal );
	SetVizMarkers( m_worldFrameId, 1.f, 1.f, 0.f, 0.5, m_unreachable_points );

	m_ofs_time = ofstream("/home/hankm/results/autoexploration/planning_time.txt");
}

FrontierDetectorDMS::~FrontierDetectorDMS()
{
	delete [] mp_cost_translation_table;
}

void FrontierDetectorDMS::initmotion( )
{
ROS_INFO("+++++++++++++++++ Start the init motion ++++++++++++++\n");
	geometry_msgs::Twist cmd_vel;
    cmd_vel.linear.x = 0.0;
    cmd_vel.linear.y = 0.0;
    cmd_vel.angular.z = 0.5;

    uint32_t start_time = ros::Time::now().sec ;
    uint32_t curr_time = start_time ;
    while( curr_time < start_time + 12 )
    {
		m_velpub.publish(cmd_vel);
		curr_time = ros::Time::now().sec ;
    }

	cmd_vel.angular.z = 0.0;
	m_velpub.publish(cmd_vel);
ROS_INFO("+++++++++++++++++ end of the init motion ++++++++++++++\n");
}


cv::Point2f FrontierDetectorDMS::gridmap2world( cv::Point img_pt_roi  )
{
	// grid_x = (map_x - map.info.origin.position.x) / map.info.resolution
	// grid_y = (map_y - map.info.origin.position.y) / map.info.resolution
	// img_x = (gridmap_x - gridmap.info.origin.position.x) / gridmap.info.resolution
	// img_y = (gridmap_y - gridmap.info.origin.position.y) / gridmap.info.resolution

	float fgx =  static_cast<float>(img_pt_roi.x) * m_fResolution + m_gridmap.info.origin.position.x  ;
	float fgy =  static_cast<float>(img_pt_roi.y) * m_fResolution + m_gridmap.info.origin.position.y  ;

	return cv::Point2f( fgx, fgy );
}

cv::Point FrontierDetectorDMS::world2gridmap( cv::Point2f grid_pt)
{
	float fx = (grid_pt.x - m_gridmap.info.origin.position.x) / m_gridmap.info.resolution ;
	float fy = (grid_pt.y - m_gridmap.info.origin.position.y) / m_gridmap.info.resolution ;

	return cv::Point( (int)fx, (int)fy );
}

int FrontierDetectorDMS::displayMapAndFrontiers( const cv::Mat& mapimg, const vector<cv::Point>& frontiers, const int winsize)
{
	//ROS_INFO("weird maprows: %d %d\n", mapimg.rows, mapimg.cols);
	if(		mapimg.empty() ||
			mapimg.rows == 0 || mapimg.cols == 0 || m_globalcostmap.info.width == 0 || m_globalcostmap.info.height == 0)
		return 0;

	float fXstartx=m_globalcostmap.info.origin.position.x; // world coordinate in the costmap
	float fXstarty=m_globalcostmap.info.origin.position.y; // world coordinate in the costmap
	float resolution = m_globalcostmap.info.resolution ;
	int cmwidth= static_cast<int>(m_globalcostmap.info.width) ;
	//auto Data=m_globalcostmap.data ;

	int x = winsize ;
	int y = winsize ;
	int width = mapimg.cols  ;
	int height= mapimg.rows  ;
	cv::Mat img = cv::Mat::zeros(height + winsize*2, width + winsize*2, CV_8UC1);

//ROS_INFO("costmap size: %d %d \n",  m_globalcostmap.info.width, m_globalcostmap.info.height);

	cv::Mat tmp = img( cv::Rect( x, y, width, height ) ) ;

ROS_INFO("mapsize: %d %d %d %d %d %d\n", tmp.cols, tmp.rows, mapimg.cols, mapimg.rows, img.cols, img.rows );

	mapimg.copyTo(tmp);
	cv::Mat dst;
	cvtColor(tmp, dst, cv::COLOR_GRAY2BGR);

	for( size_t idx = 0; idx < frontiers.size(); idx++ )
	{
		int x = frontiers[idx].x - winsize ;
		int y = frontiers[idx].y - winsize ;
		int width = winsize * 2 ;
		int height= winsize * 2 ;
		cv::rectangle( dst, cv::Rect(x,y,width,height), cv::Scalar(0,255,0) );
	}

	cv::Mat dstroi = dst( cv::Rect( mapimg.cols/4, mapimg.rows/4, mapimg.cols/2, mapimg.rows/2 ) );
	cv::pyrDown(dstroi, dstroi, cv::Size(dstroi.cols/2, dstroi.rows/2) );

//ROS_INFO("dst size: %d %d \n", dst.rows, dst.cols);
#ifdef FD_DEBUG_MODE
//	ROS_INFO("displaying dstroi \n");
//	cv::namedWindow("mapimg", 1);
//	cv::imshow("mapimg",dst);
//	cv::waitKey(30);
#endif
}


bool FrontierDetectorDMS::isValidPlan( vector<cv::Point>  )
{

}

void FrontierDetectorDMS::publishDone( )
{
	std_msgs::Bool done_task;
	done_task.data = true;
	m_donepub.publish( done_task );
}


void FrontierDetectorDMS::globalCostmapCallBack(const nav_msgs::OccupancyGrid::ConstPtr& msg)
{
	const std::unique_lock<mutex> lock(mutex_costmap);
ROS_INFO("cm callback is called \n");
	m_globalcostmap = *msg ;
	m_globalcostmap_rows = m_globalcostmap.info.height ;
	m_globalcostmap_cols = m_globalcostmap.info.width ;
}

void FrontierDetectorDMS::globalCostmapUpdateCallback(const map_msgs::OccupancyGridUpdate::ConstPtr& msg )
{
	const std::unique_lock<mutex> lock(mutex_costmap);
	map_msgs::OccupancyGridUpdate cm_updates = *msg;
	std::vector<signed char> Data=cm_updates.data;
	int x = cm_updates.x ;
	int y = cm_updates.y ;
	int height = cm_updates.height ;
	int width  = cm_updates.width ;
	int cmwidth = m_globalcostmap.info.width ;
	int dataidx = 0 ;
//ROS_INFO("cm updates size: %d \n", Data.size() );
//ROS_INFO("info: x y h w (%d %d %d %d) \n", cm_updates.x, cm_updates.y,
//			cm_updates.height, cm_updates.width  );
	{
		//const std::unique_lock<mutex> lock(mutex_global_costmap) ;

		for(int ii=0; ii < height; ii++)
		{
			for(int jj=0; jj < width; jj++)
			{
				int idx = x + jj + ii * cmwidth ;
				m_globalcostmap.data[dataidx] = Data[dataidx] ;
				dataidx++;
			}
		}
	}
}



void FrontierDetectorDMS::robotPoseCallBack( const geometry_msgs::PoseWithCovarianceStamped::ConstPtr& msg )
{
	m_robotpose = *msg ;
	//ROS_INFO("callback pose:  %f %f \n", m_robotpose.pose.pose.position.x, m_robotpose.pose.pose.position.y);
}

void FrontierDetectorDMS::robotVelCallBack( const geometry_msgs::Twist::ConstPtr& msg )
{
	m_robotvel = *msg ;
}


// mapcallback for dynamic mapsize (i.e for the cartographer)
void FrontierDetectorDMS::mapdataCallback(const nav_msgs::OccupancyGrid::ConstPtr& msg) //const octomap_server::mapframedata& msg )
{
	if(!m_isInitMotionCompleted)
	{
		ROS_WARN("FD has not fully instantiated yet !");
		return;
	}

	if(m_robotvel.linear.x == 0 && m_robotvel.angular.z == 0 ) // robot is physically stopped
		m_eRobotState = ROBOT_STATE::ROBOT_IS_NOT_MOVING;

ROS_INFO("Robot state in mapdataCallback: %d \n ",  m_eRobotState);

	if(m_eRobotState >= ROBOT_STATE::FORCE_TO_STOP )
	{
		ROS_WARN("Force to stop flag is up cannot proceed mapdataCallback() \n");
		return;
	}
ros::WallTime startTime, endTime;
startTime = ros::WallTime::now();

	float gmresolution ;
	uint32_t gmheight, gmwidth;

	nav_msgs::OccupancyGrid globalcostmap;
	float cmresolution, cmstartx, cmstarty;
	uint32_t cmwidth, cmheight;
	std::vector<signed char> cmdata;

ROS_INFO("b4 map assigned  \n ");

	{
		const std::unique_lock<mutex> lock(mutex_gridmap);
		m_gridmap = *msg ;
		gmresolution = m_gridmap.info.resolution ;
		gmheight = m_gridmap.info.height ;
		gmwidth = m_gridmap.info.width ;
	}

	{
		const std::unique_lock<mutex> lock(mutex_costmap);
		globalcostmap = m_globalcostmap;
		cmresolution=globalcostmap.info.resolution;
		cmstartx=globalcostmap.info.origin.position.x;
		cmstarty=globalcostmap.info.origin.position.y;
		cmwidth= globalcostmap.info.width;
		cmheight=globalcostmap.info.height;
		cmdata  =globalcostmap.data;
	}

ROS_INFO("after map assigned  \n ");

	if( gmheight == 0 || gmwidth == 0
		|| gmwidth  != cmwidth
		|| gmheight != cmheight)
	{
		ROS_WARN("unreliable grid map input h/w (%d, %d) gcostmap h/w (%d, %d) \n",
				gmheight, gmwidth,
				cmheight, cmwidth);
		return;
	}

//ROS_INFO("in mapdataCallback (grid map info: %f %f %d %d)\n",
//		m_gridmap.info.origin.position.x, m_gridmap.info.origin.position.y,
//		m_gridmap.info.height, m_gridmap.info.width);
//ROS_INFO("in mapdataCallback (gcostmap info: %f %f %d %d)\n",
//		cmstartx, cmstarty,	cmheight, cmwidth);

//	ROS_INFO("offset adjust: %d %d %d %d num ds: %d\n", nrows_, ncols_, m_nrows, m_ncols, m_nNumPyrDownSample );
//	m_nrows = m_gridmap.info.height ; //% 2 == 0 ? m_gridmap.info.height : m_gridmap.info.height + 1;
//	m_ncols = m_gridmap.info.width  ; //% 2 == 0 ? m_gridmap.info.height : m_gridmap.info.height + 1;
	m_nroi_origx = m_nGlobalMapCentX ; // - (int)round( m_gridmap.info.origin.position.x / m_fResolution ) ;
	m_nroi_origy = m_nGlobalMapCentY ; //- (int)round( m_gridmap.info.origin.position.y / m_fResolution ) ;

ROS_INFO("origx origy cols rows %d %d %d %d\n", m_nroi_origx, m_nroi_origy, gmwidth, gmheight);
	cv::Rect roi( m_nroi_origx, m_nroi_origy, gmwidth, gmheight );

// to make sure that img(0,0) == UNKNOWN
	//cv::Rect roi_offset( m_nroi_origx - ROI_OFFSET, m_nroi_origy - ROI_OFFSET, m_ncols + ROI_OFFSET*2, m_nrows + ROI_OFFSET*2 );

//	cv::Mat img ;
//	img = m_uMapImg(roi);
	m_uMapImgROI = m_uMapImg(roi);

	for( int ii =0 ; ii < gmheight; ii++)
	{
		for( int jj = 0; jj < gmwidth; jj++)
		{
			int8_t occupancy = m_gridmap.data[ ii * gmwidth + jj ]; // dynamic gridmap size
			int y_ = (m_nroi_origy + ii) ;
			int x_ = (m_nroi_origx + jj) ;

			if ( occupancy < 0 )
			{
				m_uMapImg.data[ y_ * m_nGlobalMapWidth + x_ ] = static_cast<uchar>(ffp::MapStatus::UNKNOWN) ;
			}
			else if( occupancy >= 0 && occupancy < m_noccupancy_thr)
			{
				m_uMapImg.data[ y_ * m_nGlobalMapWidth + x_ ] = static_cast<uchar>(ffp::MapStatus::FREE) ;
			}
			else
			{
				m_uMapImg.data[ y_ * m_nGlobalMapWidth + x_ ] = static_cast<uchar>(ffp::MapStatus::OCCUPIED) ;
			}
		}
	}
//ROS_INFO("%d %d %d %d\n", img.rows, img.cols, m_nrows, m_ncols );

//cv::namedWindow("roi",1);
//cv::imshow("roi",m_uMapImgROI);
//cv::waitKey(30);
	m_markerfrontierpub.publish(m_points); // Publish frontiers to renew Rviz
	m_makergoalpub.publish(m_exploration_goal);

// The robot is not moving (or ready to move)... we can go ahead plan the next action...
// i.e.) We locate frontier points again, followed by publishing the new goal

	ROS_INFO("******* Begin mapdataCallback procedure ******** \n");
	ROS_INFO(" # of downsamples %d \n", m_nNumPyrDownSample);

	cv::Mat img_ ;
	img_ = m_uMapImg( roi ); //m_uMapImgROI.clone();

//string strroifile = "/home/hankm/results/autoexploration/tmp/roi.txt"; //string(m_str_debugpath) + string("/tmp/img_cl.txt") ;
//ofstream ofs_roi(strroifile);
//for( int ridx=0; ridx < img.rows; ridx++ )
//{
//	for( int cidx=0; cidx < img.cols; cidx++ )
//	{
//		uint8_t val = img.data[ridx*img.cols + cidx] ;
//		uint32_t uval= static_cast<uint32_t>(val);
//		ofs_roi << uval << " ";
//	}
//	ofs_roi << endl;
//}
//ofs_roi.close();

	if( m_nNumPyrDownSample > 0)
	{
		// be careful here... using pyrDown() interpolates occ and free, making the boarder area (0 and 127) to be 127/2 !!
		// 127 reprents an occupied cell !!!
		//downSampleMap(img);
		for(int iter=0; iter < m_nNumPyrDownSample; iter++ )
		{
			int nrows = img_.rows; //% 2 == 0 ? img.rows : img.rows + 1 ;
			int ncols = img_.cols; // % 2 == 0 ? img.cols : img.cols + 1 ;
			ROS_INFO("sizes orig: %d %d ds: %d %d \n", img_.rows, img_.cols, nrows/2, ncols/2 );
			pyrDown(img_, img_, cv::Size( ncols/2, nrows/2 ) );
			//clusterToThreeLabels( img );
		}
		//cv::imwrite("/home/hankm/catkin_ws/src/frontier_detector/launch/pyr_img.png", img);
	}
	clusterToThreeLabels( img_ );

// We need to zero-pad around img b/c m_gridmap dynamically increases
	uint8_t ukn = static_cast<uchar>(ffp::MapStatus::UNKNOWN) ;
	cv::Mat img = cv::Mat( img_.rows + ROI_OFFSET*2, img_.cols + ROI_OFFSET*2, CV_8U, cv::Scalar(ukn) ) ;
	cv::Rect myroi( ROI_OFFSET, ROI_OFFSET, img_.cols, img_.rows );
	cv::Mat img_roi = img(myroi) ;
	img_.copyTo(img_roi) ;

//string strimgfile = "/home/hankm/results/autoexploration/tmp/ds_img.txt"; //string(m_str_debugpath) + string("/tmp/img_cl.txt") ;
//ofstream ofs_img(strimgfile);
//for( int ridx=0; ridx < img.rows; ridx++ )
//{
//	for( int cidx=0; cidx < img.cols; cidx++ )
//	{
//		uint8_t val = img.data[ridx*img.cols + cidx] ;
//		uint32_t uval= static_cast<uint32_t>(val);
//		ofs_img << uval << " ";
//	}
//	ofs_img << endl;
//}
//ofs_img.close();

	ffp::FrontPropagation oFP(img); // image uchar
	oFP.update(img, cv::Point(0,0));
	oFP.extractFrontierRegion( img ) ;

	cv::Mat img_frontiers = oFP.GetFrontierContour() ;

	cv::Mat dst;
	cvtColor(img_frontiers, dst, cv::COLOR_GRAY2BGR);

// locate the most closest labeled points w.r.t the centroid pts

	vector<vector<cv::Point> > contours;
	vector<cv::Vec4i> hierarchy;
	cv::findContours( img_frontiers, contours, hierarchy, CV_RETR_CCOMP, CV_CHAIN_APPROX_NONE );

#ifdef FD_DEBUG_MODE
	string outfilename =  m_str_debugpath + "/global_mapimg.png" ;
//	cv::imwrite( outfilename.c_str(), m_uMapImg);
//	cv::imwrite(m_str_debugpath + "/labeled_img.png", img);
//	cv::imwrite(m_str_debugpath + "/img_frontiers.png",img_frontiers);
#endif

	if( contours.size() == 0 )
		return;

	// iterate through all the top-level contours,
	// draw each connected component with its own random color
	int idx = 0;
	for( ; idx >= 0; idx = hierarchy[idx][0] )
	{
//ROS_INFO("hierarchy: %d \n", idx);
		cv::Scalar color( rand()&255, rand()&255, rand()&255 );
		drawContours( dst, contours, idx, color, CV_FILLED, 8, hierarchy );
	}

	vector<cv::Point2f> fcents;
	for(int i=0; i < contours.size(); i++)
	{
		float fx =0, fy =0 ;
		float fcnt = 0 ;
		vector<cv::Point> contour = contours[i];
		for( int j=0; j < contour.size(); j++)
		{
			fx += static_cast<float>( contour[j].x ) ;
			fy += static_cast<float>( contour[j].y ) ;
			fcnt += 1.0;
		}
		fx = fx/fcnt ;
		fy = fy/fcnt ;

		cv::Point2f fcent( fx,  fy ) ;
		fcents.push_back(fcent);
	}

	// get closest frontier pt to each cent
	// i.e.) the final estimated frontier points
	vector<FrontierPoint> voFrontierCands;

ROS_INFO("contours size() %d \n", contours.size() );

	for( int i = 0; i < contours.size(); i++ )
	{
		vector<cv::Point> contour = contours[i] ;

		if(contour.size() < m_frontiers_region_thr ) // don't care about small frontier regions
			continue ;

		float fcentx = fcents[i].x ;
		float fcenty = fcents[i].y ;

		float fmindist = 1000 ;
		int nmindistidx = -1;

		for (int j=0; j < contour.size(); j++)
		{
			float fx = static_cast<float>(contour[j].x) ;
			float fy = static_cast<float>(contour[j].y) ;
			float fdist = std::sqrt( (fx - fcentx) * (fx - fcentx) + (fy - fcenty) * (fy - fcenty) );
			if(fdist < fmindist)
			{
				fmindist = fdist ;
				nmindistidx = j ;
			}
		}

		CV_Assert(nmindistidx >= 0);
		cv::Point frontier = contour[nmindistidx];

		if(
			(ROI_OFFSET > 0) &&
			(frontier.x <= ROI_OFFSET || frontier.y <= ROI_OFFSET ||
			 frontier.x >= gmwidth + ROI_OFFSET || frontier.y >= gmheight + ROI_OFFSET)
		   )
		{
			continue;
		}

		frontier.x = frontier.x - ROI_OFFSET ;
		frontier.y = frontier.y - ROI_OFFSET ;

		//frontiers_cand.push_back(frontier) ;
		FrontierPoint oPoint( frontier, gmheight, gmwidth,
								m_gridmap.info.origin.position.y, m_gridmap.info.origin.position.x,
			   // m_nGlobalMapCentY, m_nGlobalMapCentX,
					   //0,0,
					   gmresolution, m_nNumPyrDownSample );

// //////////////////////////////////////////////////////////////////
// 				We need to run position correction here
/////////////////////////////////////////////////////////////////////
		cv::Point init_pt 		= oPoint.GetInitGridmapPosition() ; 	// position @ ds0 (original sized map)
		cv::Point corrected_pt	= oPoint.GetCorrectedGridmapPosition() ;
		correctFrontierPosition( m_gridmap, init_pt, m_nCorrectionWindowWidth, corrected_pt  );

		oPoint.SetCorrectedCoordinate(corrected_pt);
		voFrontierCands.push_back(oPoint);
	}

ROS_INFO("costmap msg width: %d \n", gmwidth );

	geometry_msgs::Point p;
	m_cands.points.clear();
	m_exploration_goal.points.clear();
	m_points.points.clear();
	//	m_unreachable_points.points.clear();

	const float fcm_conf = m_oFrontierFilter.GetCostmapConf() ;
	const float fgm_conf = m_oFrontierFilter.GetGridmapConf() ;

	for(size_t idx=0; idx < voFrontierCands.size(); idx++)
	{
		cv::Point2f frontier_in_world = voFrontierCands[idx].GetCorrectedWorldPosition() ;
		p.x = frontier_in_world.x ;
		p.y = frontier_in_world.y ;
		p.z = 0.0 ;
		m_cands.points.push_back(p);
		//ROS_INFO("frontier cands: %f %f \n", p.x, p.y);
	}

	// eliminate frontier points at obtacles
	vector<int> valid_frontier_indexs;
	if( globalcostmap.info.width > 0 )
	{
//ROS_INFO( "eliminating supurious frontiers \n" );
		//frontiers = eliminateSupriousFrontiers( m_globalcostmap, frontiers_cand, m_nROISize) ;

		m_oFrontierFilter.measureCostmapConfidence(globalcostmap, voFrontierCands);
		m_oFrontierFilter.measureGridmapConfidence(m_gridmap, voFrontierCands);

		for(size_t idx=0; idx < voFrontierCands.size(); idx++)
			voFrontierCands[idx].SetFrontierFlag( fcm_conf, fgm_conf );

		set<pointset, pointset> unreachable_frontiers;
		{
			const std::unique_lock<mutex> lock(mutex_unreachable_points) ;
			unreachable_frontiers = m_unreachable_frontier_set ;
			m_oFrontierFilter.computeReachability( unreachable_frontiers, voFrontierCands );

			for( size_t idx=0; idx < voFrontierCands.size(); idx++)
			{
				if( !voFrontierCands[idx].isConfidentFrontierPoint() )
					continue ;

				cv::Point frontier_in_gridmap = voFrontierCands[idx].GetCorrectedGridmapPosition();
				geometry_msgs::Point p;
				p.x = frontier_in_gridmap.x ;
				p.y = frontier_in_gridmap.y ;
				p.z = 0.0 ;
				m_unreachable_points.points.push_back(p) ;
			}
		}
	}
	else
	{
		ROS_INFO("costmap hasn't updated \n");
		//frontiers = frontiers_cand ; // points in img coord
	}

//static int mapidx = 0;
//char cgridmapfile[100];
//char costmapfile[100];
//char cfptfile[100];
//sprintf(cgridmapfile,"%s/tmp/gm%03d.txt", m_str_debugpath.c_str(), mapidx );
//std::string strgridmapfile(cgridmapfile) ;
//
//sprintf(costmapfile,"%s/tmp/cm%03d.txt", m_str_debugpath.c_str(), mapidx );
//std::string strcostmapfile(costmapfile) ;

//sprintf(cfptfile,"%s/tmp/fpt%03d.txt", m_str_debugpath.c_str(), mapidx );
//std::string strfptfile(cfptfile) ;
//
//saveGridmap( strgridmapfile, m_gridmap ) ;
//saveGridmap( strcostmapfile, m_globalcostmap ) ;
//saveFrontierCandidates( strfptfile, voFrontierCands );
//mapidx++;

#ifdef FD_DEBUG_MODE
	string strcandfile = m_str_debugpath + "/front_cand.txt" ;
	ofstream ofs_cand(strcandfile);
	for( int idx=0; idx < frontiers_cand.size(); idx++ )
	{
		ofs_cand << frontiers_cand[idx].x << " " << frontiers_cand[idx].y << endl;
	}
	ofs_cand.close();
#endif

	for (size_t idx=0; idx < voFrontierCands.size(); idx++)
	{
		if( voFrontierCands[idx].isConfidentFrontierPoint() )
			valid_frontier_indexs.push_back( idx );
	}

//	for(int idx=0; idx < frontiers.size(); idx++)
//		ROS_INFO("Valid frontier points: %f %f \n", frontiers[idx].x, frontiers[idx].y);

	if( valid_frontier_indexs.size() == 0 )
	{
		ROS_WARN("no valid frontiers \n");
		//isdone = true;
		return;
	}

//	if(img_frontiers.rows > 0 && img_frontiers.cols > 0 )
//	{
//		displayMapAndFrontiers( img_frontiers, frontiers, m_nROISize );
//	}

//	if(m_globalcostmap.info.width > 0 &&  m_globalcostmap.info.height > 0 )
//		assessFrontiers( frontiers );


	// set exploration goals
	for(size_t idx=0; idx < valid_frontier_indexs.size(); idx++)
	{
		//cv::Point frontier = frontiers[idx] ;
//ROS_INFO("frontier pts found: %d %d \n",frontier.x,frontier.y);
#ifdef FD_DEBUG_MODE
		cv::circle(dst, frontier, 3, CV_RGB(255,0,0), 2);
#endif

		// scale then conv 2 gridmap coord
		size_t vidx = valid_frontier_indexs[idx];
		cv::Point2f frontier_in_world = voFrontierCands[vidx].GetCorrectedWorldPosition();
		geometry_msgs::PoseWithCovarianceStamped mygoal ; // float64
		mygoal.header.stamp=ros::Time(0);
		mygoal.header.frame_id = m_worldFrameId;
		mygoal.pose.pose.position.x= frontier_in_world.x ;
		mygoal.pose.pose.position.y= frontier_in_world.y ;
		mygoal.pose.pose.position.z=0.0;
		//m_exploration_goal.push_back(mygoal) ;

		m_targetspub.publish(mygoal);
		p.x = mygoal.pose.pose.position.x ;
		p.y = mygoal.pose.pose.position.y ;
		p.z = 0.0 ;
		m_points.points.push_back(p);

//		ROS_INFO("frontier in img: (%d %d) in gridmap: (%f %f) scale: %d\n",
//				frontier.x, frontier.y, p.x, p.y, m_nScale );

	}

#ifdef FFD_DEBUG_MODE
		imwrite(m_str_debugpath+"/frontier_cents.png", dst);
#endif


	//ROS_INFO("costmap info: %f %f %f %f \n", resolution, Xstartx, Xstarty, width);
	//ROS_INFO("frontier: %f %f \n", m_points.points[0].x, m_points.points[0].y );

// generate a path trajectory
// call make plan service


// Here we do motion planning

ROS_INFO("setting costmap in gph \n");

	geometry_msgs::PoseStamped start = GetCurrPose( );
	start.header.frame_id = m_worldFrameId;

vector<uint32_t> plan_len;
plan_len.resize( m_points.points.size() );

//mpo_gph->setCostmap(Data, m_globalcostmap.info.width, m_globalcostmap.info.height, m_globalcostmap.info.resolution,
//					m_globalcostmap.info.origin.position.x, m_globalcostmap.info.origin.position.y) ;

ROS_INFO("resizing mpo_costmap \n");
mpo_costmap->resizeMap( cmwidth, cmheight, cmresolution,
					    cmstartx, cmstarty );
ROS_INFO("mpo_costmap has been reset \n");
unsigned char* pmap = mpo_costmap->getCharMap() ;
ROS_INFO("w h datlen : %d %d %d \n", cmwidth, cmheight, cmdata.size() );
for(uint32_t ridx = 0; ridx < cmheight; ridx++)
{
	for(uint32_t cidx=0; cidx < cmwidth; cidx++)
	{
		uint32_t idx = ridx * cmwidth + cidx ;
//ROS_INFO("here idx: %d \n", idx);
		int8_t val = cmdata[idx];
//ROS_INFO("idx val tablev : %d %d %d\t", idx, val, mp_cost_translation_table[val] );
		pmap[idx] = val < 0 ? 255 : mp_cost_translation_table[val];
//ROS_INFO(" %d %d\n", pmap[idx], mp_cost_translation_table[val]);
	}
}

ROS_INFO("mpo_costmap has been set\n");

vector< uint32_t > gplansizes( m_points.points.size(), 0 ) ;
startTime = ros::WallTime::now();
omp_set_num_threads(12);
#pragma omp parallel firstprivate( mpo_gph ) shared( mpo_costmap, gplansizes )
{
	mpo_gph = new GlobalPlanningHandler();

	#pragma omp for
	for (size_t idx=0; idx < m_points.points.size(); idx++)
	{
ROS_INFO("processing (%f %f) with thread %d/%d : %d", p.x, p.y, omp_get_thread_num(), omp_get_num_threads(), idx );
		//#pragma omp atomic

		//pogph = new GlobalPlanningHandler();
		mpo_gph->reinitialization( mpo_costmap ) ;
//ROS_INFO("setting costmap \n");
//		pogph->setCostmap(Data, m_globalcostmap.info.width, m_globalcostmap.info.height, m_globalcostmap.info.resolution,
//						m_globalcostmap.info.origin.position.x, m_globalcostmap.info.origin.position.y) ;

//ROS_INFO("costmap is set \n");
		p = m_points.points[idx];  // just for now... we need to fix it later
		geometry_msgs::PoseStamped goal = StampedPosefromSE2( p.x, p.y, 0.f );
		goal.header.frame_id = m_worldFrameId ;
		std::vector<geometry_msgs::PoseStamped> plan;
		mpo_gph->makePlan(start, goal, plan);

		gplansizes[idx] = plan.size();
//ROS_INFO("planning completed \n");
if( plan.size() > 0 )
	ROS_INFO("%d /%d th goal marked %d length plan \n", idx, m_points.points.size(), plan.size() );
else
	ROS_INFO("cannot find a valid plan to the %d / %d th goal \n",idx, m_points.points.size() );
///////////////////////////////////////////////////////////////////////////
	}
	//ROS_INFO("deleting mpo_gph \n");
	delete mpo_gph;
}

ROS_INFO("Looking for a new plan \n");

std::vector<geometry_msgs::PoseStamped> best_plan ;
size_t best_len = 100000000 ;
size_t best_idx = 0;
for(size_t idx=0; idx < gplansizes.size(); idx++ )
{
	size_t curr_len = gplansizes[idx] ;
	ROS_INFO("%u th  plan size %u \n", idx, curr_len);
	if(curr_len < best_len && curr_len > MIN_TARGET_DIST)
	{
		best_len = curr_len ;
		best_idx = idx ;
		//best_plan = plan ;
	}
}

p = m_points.points[best_idx];  // just for now... we need to fix it later
geometry_msgs::PoseStamped best_goal = StampedPosefromSE2( p.x, p.y, 0.f );
m_bestgoal.header.frame_id = m_worldFrameId ;
m_bestgoal.pose.pose = best_goal.pose ;

endTime = ros::WallTime::now();

// print results
double gp_time = (endTime - startTime).toNSec() * 1e-6;
ROS_INFO(" %u planning time \t %f \n",m_points.points.size(), gp_time);
m_ofs_time << m_points.points.size() << "    " << gp_time << endl;

	p.x = m_bestgoal.pose.pose.position.x ;
	p.y = m_bestgoal.pose.pose.position.y ;
	p.z = 0.0 ;
	m_exploration_goal.points.push_back(p);

	m_markercandpub.publish(m_cands);

	m_markerfrontierpub.publish(m_points);
//////////////////////////////////////////////////////
// lets disable publishing goal if Fetch robot is not used
	m_currentgoalpub.publish(m_bestgoal);
/////////////////////////////////////////////////////

	m_makergoalpub.publish(m_exploration_goal);

	// publish the best goal of the path plan
	ROS_INFO("@mapDataCallback start(%f %f) found the best goal(%f %f) best_len (%u)\n",
			start.pose.position.x, start.pose.position.y,
			m_bestgoal.pose.pose.position.x, m_bestgoal.pose.pose.position.y, best_len);

	{
		const std::unique_lock<mutex> lock(mutex_robot_state) ;
		m_eRobotState == ROBOT_STATE::ROBOT_IS_READY_TO_MOVE;
	}

endTime = ros::WallTime::now();

// print results
double execution_time = (endTime - startTime).toNSec() * 1e-6;
ROS_INFO("\n "
		 " ****************************************************** \n "
		 "	 mapDataCallback exec time (ms): %f \n "
		 " ****************************************************** \n "
				, execution_time);

}

void FrontierDetectorDMS::doneCB( const actionlib::SimpleClientGoalState& state )
{
    ROS_INFO("DONECB: Finished in state [%s]", state.toString().c_str());
    if (m_move_client.getState() == actionlib::SimpleClientGoalState::SUCCEEDED)
    {
         // do something as goal was reached
    	ROS_INFO("touch down  \n");
		{
			const std::unique_lock<mutex> lock(mutex_robot_state) ;
			m_eRobotState = ROBOT_STATE::ROBOT_IS_NOT_MOVING ;
		}
    }
    else if (m_move_client.getState() == actionlib::SimpleClientGoalState::ABORTED)
    {
        // do something as goal was canceled
    	ROS_ERROR("Aboarted ... \n");
		{
			const std::unique_lock<mutex> lock(mutex_robot_state) ;
			m_eRobotState = ROBOT_STATE::ROBOT_IS_NOT_MOVING ;
		}
    }
    else
    {
    	ROS_ERROR("unknown state \n");
    	exit(-1);
    }
}


void FrontierDetectorDMS::moveRobotCallback(const geometry_msgs::PoseWithCovarianceStamped::ConstPtr& msg )
{
// call actionlib
// robot is ready to move
	ROS_INFO("Robot state in moveRobotCallback: %d \n ",  m_eRobotState);

	if( m_eRobotState >= ROBOT_STATE::FORCE_TO_STOP ) //|| isdone )
		return;

	geometry_msgs::PoseWithCovarianceStamped goalpose = *msg ;

	{
		const std::unique_lock<mutex> lock(mutex_robot_state) ;
		m_eRobotState = ROBOT_STATE::ROBOT_IS_MOVING ;
	}

	ROS_INFO("@moveRobotCallback received a plan\n");

	move_base_msgs::MoveBaseGoal goal;
	goal.target_pose.header.frame_id = m_worldFrameId; //m_baseFrameId ;
	goal.target_pose.header.stamp = ros::Time::now() ;

//	geometry_msgs::PoseWithCovarianceStamped goalpose = // m_pathplan.poses.back() ;

	goal.target_pose.pose.position.x = goalpose.pose.pose.position.x ;
	goal.target_pose.pose.position.y = goalpose.pose.pose.position.y ;
	goal.target_pose.pose.orientation.w = goalpose.pose.pose.orientation.w ;

// inspect the path
//	ROS_INFO("printing path for start(%f %f) to goal(%f %f): \n",
//			req.start.pose.position.x, req.start.pose.position.y, req.goal.pose.position.x, req.goal.pose.position.y );

//	ROS_INFO("@moveRobotCallback goal pose: %f %f %f\n", goalpose.pose.pose.position.x, goalpose.pose.pose.position.y, goalpose.pose.pose.orientation.w);
//////////////////////////////////////////////////////////////////////////////////////////////
// when creating path to frontier points, navfn_ros::makePlan() is called
// move_client calls moveBase::makePlan() ...
//////////////////////////////////////////////////////////////////////////////////////////////
//ROS_INFO("+++++++++++++++++++++++++ @moveRobotCallback, sending a goal +++++++++++++++++++++++++++++++++++++\n");
	m_move_client.sendGoal(goal, boost::bind(&FrontierDetectorDMS::doneCB, this, _1), SimpleMoveBaseClient::SimpleActiveCallback() ) ;
//ROS_INFO("+++++++++++++++++++++++++ @moveRobotCallback, a goal is sent +++++++++++++++++++++++++++++++++++++\n");
	//m_move_client.sendGoalAndWait(goal, ros::Duration(0, 0), ros::Duration(0, 0)) ;
	m_move_client.waitForResult();
}

void FrontierDetectorDMS::unreachablefrontierCallback(const geometry_msgs::PoseStamped::ConstPtr& msg )
{
	ROS_INFO("Robot state in unreachablefrontierCallback: %d \n ",  m_eRobotState);

	geometry_msgs::PoseStamped unreachablepose = *msg ;
	pointset pi ;
	pi.d[0] = unreachablepose.pose.position.x ;
	pi.d[1] = unreachablepose.pose.position.y ;

	{
		const std::unique_lock<mutex> lock(mutex_unreachable_points) ;
		m_unreachable_frontier_set.insert( pi ) ;

		geometry_msgs::Point p;
		p.x = pi.d[0];
		p.y = pi.d[1];
		m_unreachable_points.points.push_back(p);
		m_unreachpointpub.publish( m_unreachable_points );
	}

	for (const auto & di : m_unreachable_frontier_set)
	    ROS_WARN("unreachable pts: %f %f\n", di.d[0], di.d[1]);

	// stop the robot and restart frontier detection procedure

	ROS_WARN("+++++++++++Cancling the unreachable goal +++++++++++++\n");

	{
		const std::unique_lock<mutex> lock(mutex_robot_state) ;
		m_eRobotState = ROBOT_STATE::FORCE_TO_STOP ;
	}

	m_move_client.stopTrackingGoal();
	m_move_client.waitForResult();
	m_move_client.cancelGoal();
	m_move_client.waitForResult();

	ROS_INFO("+++++++++++ Robot is ready for motion +++++++++++++\n");
	{
		const std::unique_lock<mutex> lock(mutex_robot_state) ;
		m_eRobotState = ROBOT_STATE::ROBOT_IS_READY_TO_MOVE ;
	}

}


}
