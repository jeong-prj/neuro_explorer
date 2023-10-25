/*********************************************************************
Copyright 2022 The Ewha Womans University.
All Rights Reserved.
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE
Permission to use, copy, modify OR distribute this software and its
documentation for educational, research and non-profit purposes, without
fee, and without a written agreement is hereby granted, provided that the
above copyright notice and the following three paragraphs appear in all
copies.

IN NO EVENT SHALL THE EWHA WOMANS UNIVERSITY BE
LIABLE TO ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
CONSEQUENTIAL DAMAGES, INCLUDING LOST PROFITS, ARISING OUT OF THE
USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN IF THE EWHA WOMANS UNIVERSITY
BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.

THE EWHA WOMANS UNIVERSITY SPECIFICALLY DISCLAIM ANY
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE
PROVIDED HEREUNDER IS ON AN "AS IS" BASIS, AND THE EWHA WOMANS UNIVERSITY
HAS NO OBLIGATIONS TO PROVIDE MAINTENANCE, SUPPORT,
UPDATES, ENHANCEMENTS, OR MODIFICATIONS.


The authors may be contacted via:


Mail:        Y. J. Kim, Kyung Min Han
             Computer Graphics Lab                       
             Department of Computer Science and Engineering
             Ewha Womans University
             11-1 Daehyun-Dong Seodaemun-gu, Seoul, Korea 120-750


Phone:       +82-2-3277-6798


EMail:       kimy@ewha.ac.kr
             hankm@ewha.ac.kr
*/

#include "frontier_detector_dms.hpp"

namespace autoexplorer
{

FrontierDetectorDMS::FrontierDetectorDMS(const ros::NodeHandle private_nh_, const ros::NodeHandle &nh_):
m_nh_private(private_nh_),
m_nh(nh_),
mn_globalcostmapidx(0), mn_numthreads(16),
mb_isinitmotion_completed(false),
mp_cost_translation_table(NULL),
mb_strict_unreachable_decision(true),
me_prev_exploration_state( SUCCEEDED ), mb_nbv_selected(false), //, mn_prev_nbv_posidx(-1)
mb_allow_unknown(true),
mn_mapcallcnt(0), mn_moverobotcnt(0),
mf_totalcallbacktime_msec(0.f), mf_totalffptime_msec(0.f), 	mf_totalplanningtime_msec(0.f), mf_totalmotiontime_msec(0.f),
mf_avgcallbacktime_msec(0.f), 	mf_avgffptime_msec(0.f), 	mf_avgplanngtime_msec(0.f), 	mf_avgmotiontime_msec(0.f),
mf_avg_fd_sessiontime_msec(0.f), mf_total_fd_sessiontime_msec(0.f),
mf_avg_astar_sessiontime_msec(0.f), mf_total_astar_sessiontime_msec(0.f),
mn_num_classes(8)
{
	m_ae_start_time = ros::WallTime::now();

	float fcostmap_conf_thr, fgridmap_conf_thr; // mf_unreachable_decision_bound ;
	int nweakcomp_threshold ;

	m_nh.getParam("/autoexplorer/debug_data_save_path", m_str_debugpath);
	m_nh.getParam("/autoexplorer/ne_report_file", mstr_report_filename);

	m_nh.param("/autoexplorer/costmap_conf_thr", fcostmap_conf_thr, 0.1f);
	m_nh.param("/autoexplorer/gridmap_conf_thr", fgridmap_conf_thr, 0.8f);
	m_nh.param("/autoexplorer/occupancy_thr", mn_occupancy_thr, 50);
	m_nh.param("/autoexplorer/lethal_cost_thr", mn_lethal_cost_thr, 80);
	m_nh.param("/autoexplorer/global_width", mn_globalmap_width, 	960) ;
	m_nh.param("/autoexplorer/global_height", mn_globalmap_height, 	960) ;
	m_nh.param("/autoexplorer/unreachable_decision_bound", mf_neighoringpt_decisionbound, 0.2f);
	m_nh.param("/autoexplorer/weak_comp_thr", nweakcomp_threshold, 10);
	m_nh.param("/autoexplorer/num_downsamples", mn_numpyrdownsample, 0);
	m_nh.param("/autoexplorer/frame_id", m_worldFrameId, std::string("map"));
	m_nh.param("/autoexplorer/strict_unreachable_decision", mb_strict_unreachable_decision, true);
	m_nh.param("/autoexplorer/allow_unknown", mb_allow_unknown, true);
	m_nh.param("/move_base/global_costmap/resolution", mf_resolution, 0.05f) ;
	m_nh.param("move_base/global_costmap/robot_radius", mf_robot_radius, 0.12); // 0.3 for fetch

	m_nh.getParam("/tf_loader/fd_model_filepath", m_str_fd_modelfilepath);
	m_nh.getParam("/tf_loader/astar_model_filepath", m_str_astar_modelfilepath);
	m_nh.param("/tf_loader/num_classes", mn_num_classes);
	//m_nh.getParam("/tf_loader/max_gmap_width", mn_max_gmap_width);
	//m_nh.getParam("/tf_loader/max_gmap_height", mn_max_gmap_height);
	mn_processed_gmap_height = mn_globalmap_height / 2 ;
	mn_processed_gmap_width = mn_globalmap_width / 2 ;

	mn_scale = pow(2, mn_numpyrdownsample);
	m_frontiers_region_thr = nweakcomp_threshold / mn_scale ;
	mn_roi_size = static_cast<int>( round( mf_robot_radius / mf_resolution ) ) * 2 ; // we never downsample costmap !!! dont scale it with roisize !!

	mn_globalmap_centx = mn_globalmap_width  / 2 ;
	mn_globalmap_centy = mn_globalmap_height / 2 ;

	//m_targetspub = m_nh.advertise<geometry_msgs::PointStamped>("detected_points", 10);
	m_currentgoalPub = m_nh.advertise<geometry_msgs::PoseWithCovarianceStamped>("curr_goalpose", 10);
	m_makergoalPub = m_nh.advertise<visualization_msgs::Marker>("curr_goal_shape",10);
	m_markercandPub = m_nh.advertise<visualization_msgs::MarkerArray>("detected_shapes", 10);
	m_markerfrontierPub = m_nh.advertise<visualization_msgs::MarkerArray>("filtered_shapes", 10);
	m_markerfrontierregionPub = m_nh.advertise<visualization_msgs::Marker>("FR_shapes", 1);
	m_marker_unreachpointPub = m_nh.advertise<visualization_msgs::MarkerArray>("unreachable_shapes", 10);
	m_unreachpointPub 		 = m_nh.advertise<geometry_msgs::PoseStamped>("unreachable_posestamped",10);

	m_velPub		= m_nh.advertise<geometry_msgs::Twist>("cmd_vel",10);
	m_donePub		= m_nh.advertise<std_msgs::Bool>("exploration_is_done",1);
	m_startmsgPub	= m_nh.advertise<std_msgs::Bool>("begin_exploration",1);
	m_otherfrontierptsPub = m_nh.advertise<nav_msgs::Path>("goal_exclusive_frontierpoints_list",1);

//	m_mapframedataSub  	= m_nh.subscribe("map", 1, &FrontierDetectorDMS::mapdataCallback, this); // kmHan
	m_mapframedataSub  	= m_nh.subscribe("move_base/global_costmap/costmap", 1, &FrontierDetectorDMS::mapdataCallback, this); // kmHan
	m_currGoalSub 		= m_nh.subscribe("curr_goalpose",1 , &FrontierDetectorDMS::moveRobotCallback, this) ; // kmHan
	//m_globalCostmapSub 	= m_nh.subscribe("move_base/global_costmap/costmap", 1, &FrontierDetectorDMS::globalCostmapCallBack, this );

	m_poseSub		   	= m_nh.subscribe("pose", 10, &FrontierDetectorDMS::robotPoseCallBack, this);
	m_velSub			= m_nh.subscribe("cmd_vel", 10, &FrontierDetectorDMS::robotVelCallBack, this);
	m_unreachablefrontierSub = m_nh.subscribe("unreachable_posestamped", 1, &FrontierDetectorDMS::unreachablefrontierCallback, this);
	m_makeplan_client = m_nh.serviceClient<nav_msgs::GetPlan>("move_base/make_plan");

	mcvu_mapimg  	  = cv::Mat(mn_globalmap_height, mn_globalmap_width, CV_8U, cv::Scalar(127));
	mcvu_costmapimg   = cv::Mat(mn_globalmap_height, mn_globalmap_width, CV_8U, cv::Scalar(255));

	int ncostmap_roi_size = mn_roi_size ; // / 2 ;
	int ngridmap_roi_size = mn_roi_size ;
	mn_correctionwindow_width = mn_scale * 2 + 1 ; // the size of the correction search window

	mo_frontierfilter = FrontierFilter(
			ncostmap_roi_size, ngridmap_roi_size, m_str_debugpath, mn_numpyrdownsample,
			fgridmap_conf_thr, fcostmap_conf_thr, mn_occupancy_thr, mn_lethal_cost_thr,
			mn_globalmap_width, mn_globalmap_height,
			mf_resolution, mf_neighoringpt_decisionbound);

	//m_oFrontierFilter.SetUnreachableDistThr( funreachable_decision_bound ) ;

	while(!m_move_client.waitForServer(ros::Duration(5.0)))
	{
		ROS_INFO("Waiting for the move_base action server to come up");
	}
	ROS_INFO("move_base action server is up");

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

// load init robot pose
	m_init_robot_pose = GetCurrRobotPose();

	// Set markers

	m_prev_frontier_set = set<pointset>();
	m_curr_frontier_set = set<pointset>();
	//m_exploration_goal = SetVizMarker( m_worldFrameId, 1.f, 0.f, 1.f, 0.5  );
	mn_FrontierID = 1;
	mn_UnreachableFptID = 0;
	ROS_INFO("autoexplorer has initialized with (%d) downsmapling map \n", mn_numpyrdownsample);

	std_msgs::Bool begin_task;
	begin_task.data = true;
	m_startmsgPub.publish( begin_task );

	m_last_oscillation_reset = ros::Time::now();

	// open ae report file
	string str_ae_report_file = m_str_debugpath + "/" + mstr_report_filename ;
ROS_INFO("report file: %s\n", str_ae_report_file.c_str());
	m_ofs_ae_report = ofstream( str_ae_report_file );

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// tf FD model
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    mptf_fd_Graph = TF_NewGraph();
    mptf_fd_Status = TF_NewStatus();
    mptf_fd_SessionOpts = TF_NewSessionOptions();
    mptf_fd_RunOpts = NULL;

    const char* tags = "serve"; // default model serving tag; can change in future
    int ntags = 1;
    mptf_fd_Session = TF_LoadSessionFromSavedModel(mptf_fd_SessionOpts, mptf_fd_RunOpts, m_str_fd_modelfilepath.c_str(), &tags, ntags, mptf_fd_Graph, NULL, mptf_fd_Status);
    if(TF_GetCode(mptf_fd_Status) == TF_OK)
    {
        printf("TF_LoadSessionFromSavedModel OK at the init state\n");
    }
    else
    {
        printf("%s",TF_Message(mptf_fd_Status));
    }

    //****** Get input tensor
    //TODO : need to use saved_model_cli to read saved_model arch
    int NumInputs = 1;
    mptf_fd_input = (TF_Output*)malloc(sizeof(TF_Output) * NumInputs);

    mtf_fd_t0 = {TF_GraphOperationByName(mptf_fd_Graph, "serving_default_input_1"), 0};
    if(mtf_fd_t0.oper == NULL)
        printf("ERROR: Failed TF_GraphOperationByName serving_default_input_1\n");
    else
	printf("TF_GraphOperationByName serving_default_input_1 is OK\n");

    mptf_fd_input[0] = mtf_fd_t0;

    //********* Get Output tensor
    int NumOutputs = 1;
    mptf_fd_output = (TF_Output*)malloc(sizeof(TF_Output) * NumOutputs);

    mtf_fd_t2 = {TF_GraphOperationByName(mptf_fd_Graph, "StatefulPartitionedCall"), 0};
    if(mtf_fd_t2.oper == NULL)
        printf("ERROR: Failed TF_GraphOperationByName StatefulPartitionedCall\n");
    else
	printf("TF_GraphOperationByName StatefulPartitionedCall is OK\n");

    mptf_fd_output[0] = mtf_fd_t2;

    //*** allocate data for inputs & outputs
    mpptf_fd_input_values = (TF_Tensor**)malloc(sizeof(TF_Tensor*)*NumInputs);
    mpptf_fd_output_values = (TF_Tensor**)malloc(sizeof(TF_Tensor*)*NumOutputs);

    //*** data allocation
    mpf_fd_data = new float[ (mn_processed_gmap_height) * (mn_processed_gmap_width) ];
    mpf_fd_result = new float[ (mn_processed_gmap_height) * (mn_processed_gmap_width) ];

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// tf astar model
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	mptf_astar_Graph = TF_NewGraph();
	mptf_astar_Status = TF_NewStatus();
	mptf_astar_SessionOpts = TF_NewSessionOptions();
	mptf_astar_RunOpts = NULL;

	mptf_astar_Session = TF_LoadSessionFromSavedModel(mptf_astar_SessionOpts, mptf_astar_RunOpts, m_str_astar_modelfilepath.c_str(), &tags, ntags, mptf_astar_Graph, NULL, mptf_astar_Status);
	if(TF_GetCode(mptf_astar_Status) == TF_OK)
	{
		printf("TF_LoadSessionFromSavedModel OK at the init state\n");
	}
	else
	{
		printf("%s",TF_Message(mptf_astar_Status));
	}

	//****** Get input tensor
	//TODO : need to use saved_model_cli to read saved_model arch
	mptf_astar_input = (TF_Output*)malloc(sizeof(TF_Output) * NumInputs);
	mtf_astar_t0 = {TF_GraphOperationByName(mptf_astar_Graph, "serving_default_input_1"), 0};
	if(mtf_astar_t0.oper == NULL)
		printf("ERROR: Failed TF_GraphOperationByName serving_default_input_1\n");
	else
	printf("TF_GraphOperationByName serving_default_input_1 is OK\n");

	mptf_astar_input[0] = mtf_astar_t0;

	//********* Get Output tensor
	mptf_astar_output = (TF_Output*)malloc(sizeof(TF_Output) * NumOutputs);
	mtf_astar_t2 = {TF_GraphOperationByName(mptf_astar_Graph, "StatefulPartitionedCall"), 0};
	if(mtf_astar_t2.oper == NULL)
		printf("ERROR: Failed TF_GraphOperationByName StatefulPartitionedCall\n");
	else
	printf("TF_GraphOperationByName StatefulPartitionedCall is OK\n");

	mptf_astar_output[0] = mtf_astar_t2;

	//*** allocate data for inputs & outputs
	mpptf_astar_input_values = (TF_Tensor**)malloc(sizeof(TF_Tensor*)*NumInputs);
	mpptf_astar_output_values = (TF_Tensor**)malloc(sizeof(TF_Tensor*)*NumOutputs);

	//*** data allocation
	mpf_astar_data = new float[ mn_processed_gmap_height * mn_processed_gmap_width * 3 ];
	mpf_astar_result = new float[ mn_processed_gmap_height * mn_processed_gmap_width * 3 ];

	// data handler
	m_data_handler = ImageDataHandler( mn_processed_gmap_height, mn_processed_gmap_width, 0.5f );

	ROS_INFO("model num classes: %d \n", mn_num_classes);

}

FrontierDetectorDMS::~FrontierDetectorDMS()
{
	delete [] mp_cost_translation_table;
//Free tensorflow instances
	free(mpptf_fd_input_values);
	free(mpptf_fd_output_values);
	free(mptf_fd_output);
	free(mptf_fd_input);
	delete [] mpf_fd_data ;
	delete [] mpf_fd_result ;

	free(mpptf_astar_input_values);
	free(mpptf_astar_output_values);
	free(mptf_astar_output);
	free(mptf_astar_input);
	delete [] mpf_astar_data ;
	delete [] mpf_astar_result ;
}

void FrontierDetectorDMS::initmotion(  const float& fvx = 0.f, const float& fvy = 0.f, const float& ftheta = 1.f  )
{
ROS_INFO("+++++++++++++++++ Start the init motion ++++++++++++++\n");
	geometry_msgs::Twist cmd_vel;
    cmd_vel.linear.x = fvx;
    cmd_vel.linear.y = fvy;
    cmd_vel.angular.z = ftheta;

    uint32_t start_time = ros::Time::now().sec ;
    uint32_t curr_time = start_time ;
    while( curr_time < start_time + 4 )
    {
		m_velPub.publish(cmd_vel);
		curr_time = ros::Time::now().sec ;
		ros::Duration(0.1).sleep();
    }

	cmd_vel.angular.z = 0.0;
	m_velPub.publish(cmd_vel);
ROS_INFO("+++++++++++++++++ end of the init motion ++++++++++++++\n");
}


cv::Point2f FrontierDetectorDMS::gridmap2world( cv::Point img_pt_roi  )
{
	float fgx =  static_cast<float>(img_pt_roi.x) * m_gridmap.info.resolution + m_gridmap.info.origin.position.x  ;
	float fgy =  static_cast<float>(img_pt_roi.y) * m_gridmap.info.resolution + m_gridmap.info.origin.position.y  ;

	return cv::Point2f( fgx, fgy );
}

cv::Point FrontierDetectorDMS::world2gridmap( cv::Point2f grid_pt)
{
	float fx = (grid_pt.x - m_gridmap.info.origin.position.x) / m_gridmap.info.resolution ;
	float fy = (grid_pt.y - m_gridmap.info.origin.position.y) / m_gridmap.info.resolution ;

	return cv::Point( (int)fx, (int)fy );
}

void FrontierDetectorDMS::generateGridmapFromCostmap( )
{
	m_gridmap = m_globalcostmap ;
	m_gridmap.info = m_globalcostmap.info ;

	int gmheight = m_globalcostmap.info.height ;
	int gmwidth = m_globalcostmap.info.width ;

	for( int ii =0 ; ii < gmheight; ii++)
	{
		for( int jj = 0; jj < gmwidth; jj++)
		{
			int8_t obs_cost  = m_globalcostmap.data[ ii * gmwidth + jj] ;

			if ( obs_cost < 0) // if unknown
			{
				m_gridmap.data[ ii*gmwidth + jj ] = -1 ;
			}
			else if( obs_cost > 97 ) // mp_cost_translation_table[51:98] : 130~252 : possibly circumscribed ~ inscribed
			{
				m_gridmap.data[ ii*gmwidth + jj] = 100 ;
			}
			else
			{
				m_gridmap.data[ ii*gmwidth + jj] = 0 ;
			}
		}
	}
	//ROS_INFO("cmap: ox oy: %f %f \n W H: %d %d", m_globalcostmap.info.origin.position.x, m_globalcostmap.info.origin.position.y, m_globalcostmap.info.width, m_globalcostmap.info.height);
	//ROS_INFO("gmap: ox oy: %f %f \n W H: %d %d", m_gridmap.info.origin.position.x, m_gridmap.info.origin.position.y, m_gridmap.info.width, m_gridmap.info.height);
}

int FrontierDetectorDMS::displayMapAndFrontiers( const cv::Mat& mapimg, const vector<cv::Point>& frontiers, const int winsize)
{
	if(		mapimg.empty() ||
			mapimg.rows == 0 || mapimg.cols == 0 || m_globalcostmap.info.width == 0 || m_globalcostmap.info.height == 0)
		return 0;

	float fXstartx=m_globalcostmap.info.origin.position.x; // world coordinate in the costmap
	float fXstarty=m_globalcostmap.info.origin.position.y; // world coordinate in the costmap
	float resolution = m_globalcostmap.info.resolution ;
	int cmwidth= static_cast<int>(m_globalcostmap.info.width) ;

	int x = winsize ;
	int y = winsize ;
	int width = mapimg.cols  ;
	int height= mapimg.rows  ;
	cv::Mat img = cv::Mat::zeros(height + winsize*2, width + winsize*2, CV_8UC1);

	cv::Mat tmp = img( cv::Rect( x, y, width, height ) ) ;

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

void FrontierDetectorDMS::publishDoneExploration( )
{
	double favg_callback_time = mf_totalcallbacktime_msec / (double)(mn_mapcallcnt) ;

	double favg_ffp_time = mf_totalffptime_msec / (double)(mn_mapcallcnt) ;
	double favg_fd_session_time = mf_total_fd_sessiontime_msec / (double)(mn_mapcallcnt) ;

	double favg_planning_time = mf_totalplanningtime_msec / (double)(mn_mapcallcnt) ;
	double favg_astar_session_time = mf_total_astar_sessiontime_msec / (double)(mn_mapcallcnt) ;

	double favg_moverobot_time = mf_totalmotiontime_msec / (double)(mn_moverobotcnt) ;

	ros::WallTime ae_end_time = ros::WallTime::now();
	double total_exploration_time = (ae_end_time - m_ae_start_time ).toNSec() * 1e-6;



	m_ofs_ae_report << "******************* exploration report ************************" << endl;
	m_ofs_ae_report << "total num of mapcallback         : " << mn_mapcallcnt << endl;
	m_ofs_ae_report << "total num of move robot cnt      : " << mn_moverobotcnt << endl;
	m_ofs_ae_report << "tot / avg callback time     (ms) : " << mf_totalcallbacktime_msec << "\t" << favg_callback_time  << endl;

	m_ofs_ae_report << "tot / avg fr pred time 	    (ms) : " << mf_totalffptime_msec << "\t" << favg_ffp_time  << endl;
	m_ofs_ae_report << "tot / avg fr session time   (ms) : " << mf_total_fd_sessiontime_msec << "\t" << favg_fd_session_time  << endl;

	m_ofs_ae_report << "tot / avg potmap pred time  (ms) : " << mf_totalplanningtime_msec << "\t" << favg_planning_time  << endl;
	m_ofs_ae_report << "tot / avg A* session time   (ms) : " << mf_total_astar_sessiontime_msec << "\t" << favg_astar_session_time  << endl;

	m_ofs_ae_report << "tot / avg motion time       (s)  : " << mf_totalmotiontime_msec / 1000.0 << "\t" << favg_moverobot_time / 1000.0 << endl;
	m_ofs_ae_report << endl;
	m_ofs_ae_report << "total exploration time      (s)  : " << total_exploration_time / 1000.0 << endl;
	m_ofs_ae_report.close();

	ROS_INFO("total map data callback counts : %d \n", mn_mapcallcnt  );
	ROS_INFO("total callback time (sec) %f \n", mf_totalcallbacktime_msec / 1000 );
	ROS_INFO("total planning time (sec) %f \n", mf_totalplanningtime_msec / 1000 );
	ROS_INFO("avg callback time (msec) %f \n", favg_callback_time  );
	ROS_INFO("avg planning time (msec) %f \n", favg_planning_time  );

	ROS_INFO("The exploration task is done... publishing -done- msg" );
	std_msgs::Bool done_task;
	done_task.data = true;

	m_frontierpoint_markers = visualization_msgs::MarkerArray() ;
	m_markerfrontierPub.publish(m_frontierpoint_markers);
	m_targetgoal_marker = visualization_msgs::Marker() ;
	m_makergoalPub.publish(m_targetgoal_marker); // for viz

	m_donePub.publish( done_task );

	ros::spinOnce();
}

void FrontierDetectorDMS::publishFrontierPointMarkers()
{
	visualization_msgs::MarkerArray ftmarkers_old = m_frontierpoint_markers ;
	for(size_t idx=0; idx < ftmarkers_old.markers.size(); idx++)
		ftmarkers_old.markers[idx].action = visualization_msgs::Marker::DELETE; //SetVizMarker( idx, visualization_msgs::Marker::DELETE, 0.f, 0.f, 0.5, "map", 0.f, 1.f, 0.f );
	m_markerfrontierPub.publish(ftmarkers_old);
	m_frontierpoint_markers.markers.resize(0);

	// publish fpts to Rviz
	for (const auto & pi : m_curr_frontier_set)
	{
		visualization_msgs::Marker vizmarker = SetVizMarker( mn_FrontierID, visualization_msgs::Marker::ADD, pi.p[0], pi.p[1], 0.f, m_worldFrameId, 0.f, 1.f, 0.f, (float)FRONTIER_MARKER_SIZE );
		m_frontierpoint_markers.markers.push_back(vizmarker);
		mn_FrontierID++ ;
	}
	m_markerfrontierPub.publish(m_frontierpoint_markers);
}

void FrontierDetectorDMS::publishFrontierRegionMarkers( const visualization_msgs::Marker& vizfrontier_regions  )
{
	// publish fpt regions to Rviz
	m_markerfrontierregionPub.publish(vizfrontier_regions);
}

void FrontierDetectorDMS::publishGoalPointMarker( const geometry_msgs::PoseWithCovarianceStamped& targetgoal )
{
	m_targetgoal_marker.points.clear();
	m_targetgoal_marker = SetVizMarker( -1, visualization_msgs::Marker::ADD, targetgoal.pose.pose.position.x, targetgoal.pose.pose.position.y, 0.f,
			m_worldFrameId,	1.f, 0.f, 1.f, (float)TARGET_MARKER_SIZE );
	m_makergoalPub.publish(m_targetgoal_marker); // for viz
}

void FrontierDetectorDMS::publishUnreachableMarkers( ) //const geometry_msgs::PoseStamped& unreachablepose )
{
	// first we refresh/update viz markers
	visualization_msgs::MarkerArray ftmarkers_old = m_unreachable_markers ;
	for(size_t idx=0; idx < ftmarkers_old.markers.size(); idx++)
		ftmarkers_old.markers[idx].action = visualization_msgs::Marker::DELETE; //SetVizMarker( idx, visualization_msgs::Marker::DELETE, 0.f, 0.f, 0.5, "map", 0.f, 1.f, 0.f );
	m_marker_unreachpointPub.publish(ftmarkers_old);
	m_unreachable_markers.markers.resize(0);

	const std::unique_lock<mutex> lock_unrc(mutex_unreachable_frontier_set) ;
	{
		// create new markers and publish them to Rviz
		for (const auto & pi : m_unreachable_frontier_set)
		{
			visualization_msgs::Marker vizmarker = SetVizMarker( mn_UnreachableFptID, visualization_msgs::Marker::ADD, pi.p[0], pi.p[1], 0.f, m_worldFrameId, 1.f, 1.f, 0.f, (float)UNREACHABLE_MARKER_SIZE);
			m_unreachable_markers.markers.push_back(vizmarker);
			mn_UnreachableFptID++ ;
		}
	}
	m_marker_unreachpointPub.publish(m_unreachable_markers);
}

void FrontierDetectorDMS::appendUnreachablePoint( const geometry_msgs::PoseStamped& unreachablepose )
{
	pointset ufpt(unreachablepose.pose.position.x, unreachablepose.pose.position.y );
	// append the incoming unreachable fpt to the list
	if( !mb_strict_unreachable_decision)
	{
		// this case, we only check the frontier sanity check... don't care about physical path plan to the target.
		// if still valid frontier, we don't register this point to unreachable list b/c we think MoveBase() can guide the robot to the goal. Yet, Robot will stuck if this is not the case.
		nav_msgs::OccupancyGrid globalcostmap;
		std::vector<signed char> cmdata;
		{
			const std::unique_lock<mutex> lock(mutex_costmap);
			globalcostmap = m_globalcostmap;
			cmdata = globalcostmap.data;
		}
		int ncmx = static_cast<int>( (ufpt.p[0] - globalcostmap.info.origin.position.x) / globalcostmap.info.resolution ) ;
		int ncmy = static_cast<int>( (ufpt.p[1] - globalcostmap.info.origin.position.y) / globalcostmap.info.resolution ) ;
		//if( frontier_sanity_check(ncmx, ncmy, globalcostmap.info.width, cmdata) )
		if( cmdata[ ncmy * globalcostmap.info.width + ncmx ] == -1 )
			return ;
	}

	{
		const std::unique_lock<mutex> lock_unrc(mutex_unreachable_frontier_set) ;
		m_unreachable_frontier_set.insert( ufpt ) ;
		ROS_WARN("@unreachablefrontierCallback Registering (%f %f) as the unreachable pt. Tot unreachable pts: %d \n", ufpt.p[0], ufpt.p[1], m_unreachable_frontier_set.size() );
	}

}

void FrontierDetectorDMS::updatePrevFrontierPointsList( )
{
	{
		const std::unique_lock<mutex> lock_curr(mutex_curr_frontier_set);
		const std::unique_lock<mutex> lock_prev(mutex_prev_frontier_set);
		set<pointset> frontier_set_tmp ;
		frontier_set_tmp = m_curr_frontier_set ;
		m_prev_frontier_set.clear() ;
		m_curr_frontier_set.clear() ;
		m_prev_frontier_set = frontier_set_tmp;
	}
}


void FrontierDetectorDMS::globalCostmapCallBack(const nav_msgs::OccupancyGrid::ConstPtr& msg)
{
	//ROS_INFO("@globalCostmapCallBack \n");
	const std::unique_lock<mutex> lock(mutex_costmap);
//ROS_INFO("cm callback is called \n");
	m_globalcostmap = *msg ;
	mu_cmheight = m_globalcostmap.info.height ;
	mu_cmwidth = m_globalcostmap.info.width ;
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
	{
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
}

void FrontierDetectorDMS::robotVelCallBack( const geometry_msgs::Twist::ConstPtr& msg )
{
	m_robotvel = *msg ;
}

int FrontierDetectorDMS::saveMap( const nav_msgs::OccupancyGrid& map, const string& infofilename, const string& mapfilename )
{
	int nwidth  = map.info.width ;
	int nheight = map.info.height ;
	float fox = map.info.origin.position.x ;
	float foy = map.info.origin.position.y ;
	float fres = map.info.resolution ;
	std::vector<signed char> cmdata;
	cmdata = map.data ;

	if( nwidth == 0 || nheight == 0 )
	{
		ROS_ERROR("Cannot save incomplete costmap \n");
		return -1;
	}

	geometry_msgs::PoseStamped robotpose = GetCurrRobotPose();
	float frx_w = (robotpose.pose.position.x - m_gridmap.info.origin.position.x) / m_gridmap.info.resolution ;
	float fry_w = (robotpose.pose.position.y - m_gridmap.info.origin.position.y) / m_gridmap.info.resolution ;
	int nrx_g = static_cast<int>(frx_w) ;
	int nry_g = static_cast<int>(fry_w) ;
	//int8_t cost = cmdata[ nwidth * nry_g + nrx_g ] ;

	std::ofstream ofs_info( infofilename );
	std::ofstream ofs_map(mapfilename);

	std::vector<signed char> mapdata =map.data;
	ofs_info << nwidth << " " << nheight << " " << fox << " " << foy << " " << fres << " " <<
			robotpose.pose.position.x << " " << robotpose.pose.position.y << endl;

	for( int ii=0; ii < nheight; ii++ )
	{
		for( int jj=0; jj < nwidth; jj++ )
		{
			int dataidx = ii * nwidth + jj ;
			int val = static_cast<int>( mapdata[ dataidx ] ) ;
			ofs_map << val << " ";
		}
		ofs_map << "\n";
	}
	ofs_map.close();
	return 1;
}

// save prev
int FrontierDetectorDMS::saveFrontierPoints( const nav_msgs::OccupancyGrid& map, const nav_msgs::Path& msg_frontiers, int bestidx, const string& frontierfile  )
{

	int nwidth  = map.info.width ;
	int nheight = map.info.height ;
	float fox = map.info.origin.position.x ;
	float foy = map.info.origin.position.y ;
	float fres = map.info.resolution ;
	std::vector<signed char> cmdata;
	cmdata = map.data ;


	std::ofstream ofs_fpts( frontierfile );
	ofs_fpts << msg_frontiers.poses.size() << " " << bestidx << endl;
	{
		// append valid previous frontier points by sanity check
		for (const auto & pi : msg_frontiers.poses)
		{
			int ngmx = static_cast<int>( (pi.pose.position.x - fox) / fres ) ;
			int ngmy = static_cast<int>( (pi.pose.position.y - fox) / fres ) ;

			//int8_t cost = cmdata[ nwidth * ngmy + ngmx ] ;

			ofs_fpts << pi.pose.position.x << " " << pi.pose.position.y << " " << ngmx << " " << ngmy << "\n"; // << cost << "\n" ;
		}
	}

	ofs_fpts.close();

	return 1;
}

int FrontierDetectorDMS::savefrontiercands( const nav_msgs::OccupancyGrid& map, const vector<FrontierPoint>& voFrontierPoints, const string& frontierfile )
{

	int nwidth  = map.info.width ;
	int nheight = map.info.height ;
	float fox = map.info.origin.position.x ;
	float foy = map.info.origin.position.y ;
	float fres = map.info.resolution ;

	std::ofstream ofs_currfpts( frontierfile );

	for(const auto & fpt : voFrontierPoints)
	{
		ofs_currfpts <<
		fpt.GetCorrectedWorldPosition().x << " " << fpt.GetCorrectedWorldPosition().y << " " <<
		fpt.GetCorrectedGridmapPosition().x << " " << fpt.GetCorrectedGridmapPosition().y << " " <<
		fpt.GetCMConfidence() << " " << fpt.GetGMConfidence() << "\n" ;
	}
	ofs_currfpts.close();

	return 1;
}




int FrontierDetectorDMS::frontier_summary( const vector<FrontierPoint>& voFrontierCurrFrame )
{
	ROS_INFO("\n========================================================================== \n"
			  "============================Frontier Summary  =============================\n\n");
	ROS_INFO("\n\n frontier found in curr frame: \n\n");
	for(size_t idx=0; idx < voFrontierCurrFrame.size(); idx++)
	{
		FrontierPoint fpt= voFrontierCurrFrame[idx];
		if(fpt.isConfidentFrontierPoint())
		{
			ROS_INFO(" (%f %f) is (Reachablity: %s) (Frontierness: %s) \n",
				fpt.GetCorrectedWorldPosition().x, fpt.GetCorrectedWorldPosition().y, fpt.isReachable()?"true":"false", fpt.isConfidentFrontierPoint()?"true":"false" );
		}
		else
		{
			ROS_ERROR(" (%f %f) is (Reachablity: %s) (Frontierness: %s) \n",
				fpt.GetCorrectedWorldPosition().x, fpt.GetCorrectedWorldPosition().y, fpt.isReachable()?"true":"false", fpt.isConfidentFrontierPoint()?"true":"false" );
		}
	}

	ROS_INFO("\n\n Tot cumulated frontier points to process: \n\n");
	for (const auto & di : m_curr_frontier_set )
	{
			ROS_INFO("cumulated pts: (%f %f) \n", di.p[0], di.p[1]);
	}

	ROS_INFO("========================================================================== \n\n");

	return 1;
}


void FrontierDetectorDMS::updateUnreachablePointSet( const nav_msgs::OccupancyGrid& globalcostmap )
{
	std::vector<signed char> cmdata = globalcostmap.data ;
	{
		const std::unique_lock<mutex> lock_unrc(mutex_unreachable_frontier_set) ;
		auto it = m_unreachable_frontier_set.begin() ;
		while (it != m_unreachable_frontier_set.end() )
		{
			auto it_element = it++;
			int ncmx = static_cast<int>( ((*it_element).p[0] - globalcostmap.info.origin.position.x) / globalcostmap.info.resolution ) ;
			int ncmy = static_cast<int>( ((*it_element).p[1] - globalcostmap.info.origin.position.y) / globalcostmap.info.resolution ) ;

			int gmidx = globalcostmap.info.width * ncmy	+ ncmx ;
			bool bisexplored = cmdata[gmidx] >=0 ? true : false ;

			//bool bisexplored = is_explored(ncmx, ncmy, globalcostmap.info.width, cmdata) ;

			if( bisexplored )
			{
				ROS_DEBUG("removing ufpt (%f %f) from the unreachable fpt list \n", (*it_element).p[0], (*it_element).p[1]) ;
				m_unreachable_frontier_set.erase(it_element);
			}
		}
	}

}

int FrontierDetectorDMS::selectNextBestPoint( const geometry_msgs::PoseStamped& robotpose, const nav_msgs::Path& goalexclusivefpts, geometry_msgs::PoseStamped& nextbestpoint  )
{
	std::vector<cv::Point2f> cvfrontierpoints;
	cv::Point2f cvrobotpoint( robotpose.pose.position.x, robotpose.pose.position.y );

	for (const auto & pi : goalexclusivefpts.poses)
		cvfrontierpoints.push_back(  cv::Point2f(pi.pose.position.x, pi.pose.position.y) );

	std::sort( begin(cvfrontierpoints), end(cvfrontierpoints), [cvrobotpoint](const cv::Point2f& lhs, const cv::Point2f& rhs)
			{ return FrontierDetectorDMS::euc_dist(cvrobotpoint, lhs) < FrontierDetectorDMS::euc_dist(cvrobotpoint, rhs); });

	for (const auto & pi : cvfrontierpoints )
	{
		float fdist = euc_dist( pi, cvrobotpoint ) ;
		//ROS_INFO("dist to alternative goals (%f %f) : %f \n", pi.x, pi.y, fdist );
	}

//	ROS_ASSERT( ++mn_prev_nbv_posidx >= 0 ); // to prev possible oscillation in selecting next best point

	ROS_WARN("The next best target is < %f %f > \n", cvfrontierpoints[0].x, cvfrontierpoints[0].y);

	nextbestpoint.pose.position.x = cvfrontierpoints[0].x ;
	nextbestpoint.pose.position.y = cvfrontierpoints[0].y ;
	nextbestpoint.header.frame_id = m_worldFrameId ;
	nextbestpoint.header.stamp = ros::Time::now();

	return 1;
}


int FrontierDetectorDMS::selectEscapingPoint( geometry_msgs::PoseStamped& escapepoint)
{
	//
}

int FrontierDetectorDMS::moveBackWard()
{
	ROS_INFO("+++++++++++++++++ moving backward  ++++++++++++++\n");
	geometry_msgs::Twist cmd_vel;
	cmd_vel.linear.x = -0.5;
	cmd_vel.linear.y = 0.0;
	cmd_vel.angular.z = 0.0;

	uint32_t start_time = ros::Time::now().sec ;
	uint32_t curr_time = start_time ;
	while( curr_time < start_time + 2 )
	{
		m_velPub.publish(cmd_vel);
		curr_time = ros::Time::now().sec ;
	}
	cmd_vel.angular.x = 0.0;
	m_velPub.publish(cmd_vel);
	ROS_INFO("+++++++++++++++++ end of moving backward +++++++++\n");
}



// mapcallback for dynamic mapsize (i.e for the cartographer)
void FrontierDetectorDMS::mapdataCallback(const nav_msgs::OccupancyGrid::ConstPtr& msg) //const octomap_server::mapframedata& msg )
{

ROS_INFO("********** \t start mapdata callback routine \t ********** \n");

ros::WallTime	mapCallStartTime = ros::WallTime::now();
	//ROS_INFO("@ mapdataCallback() ");

	if(!mb_isinitmotion_completed)
	{
		ROS_WARN("FD has not fully instantiated yet !");
		return;
	}

	if(m_robotvel.linear.x == 0 && m_robotvel.angular.z == 0 ) // robot is physically stopped
	{
		const std::unique_lock<mutex> lock(mutex_robot_state) ;
		me_robotstate = ROBOT_STATE::ROBOT_IS_NOT_MOVING;
	}

	nav_msgs::OccupancyGrid globalcostmap;

	m_globalcostmap = *msg ;

//ROS_INFO("costmap orig %f %f %f\n",
//		m_globalcostmap.info.origin.position.x,
//		m_globalcostmap.info.origin.position.y,
//		m_globalcostmap.info.origin.position.z
//		);
//	if( abs(m_globalcostmap.info.origin.position.x) < 0.001 || abs(m_globalcostmap.info.origin.position.y) < 0.001 )
//	{
//		// check if we r exploring a simulator
//		if( abs(m_init_robot_pose.pose.position.x) > 0 && abs(m_init_robot_pose.pose.position.y) > 0  )
//		{
//			ROS_WARN(" origin shouldn't be at <0, 0> in the simulator \n");
//			return ;
//		}
//	}

	globalcostmap = m_globalcostmap;

ROS_INFO("got costmap size of %d %d \n", m_globalcostmap.info.height, m_globalcostmap.info.width );

	generateGridmapFromCostmap();

	float cmresolution=globalcostmap.info.resolution;
	float gmresolution= cmresolution ;

	float cmstartx=globalcostmap.info.origin.position.x;
	float cmstarty=globalcostmap.info.origin.position.y;
	uint32_t cmwidth =globalcostmap.info.width;
	uint32_t cmheight=globalcostmap.info.height;
	std::vector<signed char> cmdata, gmdata;

	cmdata =globalcostmap.data;
	gmdata = m_gridmap.data;

	// set the cent of the map as the init robot position (x, y)
	//cv::Point Offset = compute_rpose_wrt_maporig() ;
	cv::Point Offset = world2gridmap( cv::Point2f( 0.f, 0.f ) ) ; // (0, 0) loc w.r.t the gmap orig (left, top)

	mn_roi_origx = mn_globalmap_centx - Offset.x; // ox in gmap coordinate when the cent of gmap is the map orig
	mn_roi_origy = mn_globalmap_centy - Offset.y; // ox in gmap coordinate when the cent of gmap is the map orig
	cv::Rect roi( mn_roi_origx, mn_roi_origy, cmwidth, cmheight );

	//mcvu_mapimgroi = mcvu_mapimg(roi);
//ROS_INFO("roi: %d %d %d %d \n", mn_roi_origx, mn_roi_origy, cmwidth, cmheight );
//ROS_INFO("ox oy: %f %f \n", globalcostmap.info.origin.position.x, globalcostmap.info.origin.position.y) ;
	for( int ii =0 ; ii < cmheight; ii++)
	{
		for( int jj = 0; jj < cmwidth; jj++)
		{
			int8_t occupancy = m_gridmap.data[ ii * cmwidth + jj ]; // dynamic gridmap size
			int8_t obs_cost  = globalcostmap.data[ ii * cmwidth + jj] ;
			int y_ = (mn_roi_origy + ii) ;
			int x_ = (mn_roi_origx + jj) ;

			if ( occupancy < 0 && obs_cost < 0)
			{
				mcvu_mapimg.data[ y_ * mn_globalmap_width + x_ ] = static_cast<uchar>(dffp::MapStatus::UNKNOWN) ;
			}
			else if( occupancy >= 0 && occupancy < mn_occupancy_thr && obs_cost < 98) // mp_cost_translation_table[51:98] : 130~252 : possibly circumscribed ~ inscribed
			{
				mcvu_mapimg.data[ y_ * mn_globalmap_width + x_ ] = static_cast<uchar>(dffp::MapStatus::FREE) ;
			}
			else
			{
				mcvu_mapimg.data[ y_ * mn_globalmap_width + x_ ] = static_cast<uchar>(dffp::MapStatus::OCCUPIED) ;
			}
		}
	}

//ROS_INFO("mcvu_mapimg has been processed %d %d \n", mcvu_mapimg.rows, mcvu_mapimg.cols );

// process costmap
	for( int ii =0 ; ii < cmheight; ii++)
	{
		for( int jj = 0; jj < cmwidth; jj++)
		{
			int8_t val  = globalcostmap.data[ ii * cmwidth + jj] ;
			int y_ = (mn_roi_origy + ii) ;
			int x_ = (mn_roi_origx + jj) ;
			mcvu_costmapimg.data[ y_ * mn_globalmap_width + x_ ] = 	val < 0 ? 255 : mp_cost_translation_table[val];
		}
	}

// The robot is not moving (or ready to move)... we can go ahead plan the next action...
// i.e.) We locate frontier points again, followed by publishing the new goal

	cv::Mat img_ ; // partial (on reconstructing map)
	img_ = mcvu_mapimg( roi );

//	if( mn_numpyrdownsample > 0)
//	{
//		// be careful here... using pyrDown() interpolates occ and free, making the boarder area (0 and 127) to be 127/2 !!
//		// 127 reprents an occupied cell !!!
//		for(int iter=0; iter < mn_numpyrdownsample; iter++ )
//		{
//			int nrows = img_.rows; //% 2 == 0 ? img.rows : img.rows + 1 ;
//			int ncols = img_.cols; // % 2 == 0 ? img.cols : img.cols + 1 ;
//			//ROS_INFO("sizes orig: %d %d ds: %d %d \n", img_.rows, img_.cols, nrows/2, ncols/2 );
//			pyrDown(img_, img_, cv::Size( ncols/2, nrows/2 ) );
//		}
//	}
//	clusterToThreeLabels( img_ );
/*******************************************************************************************************************************************
 We need to zero-pad around img b/c m_gridmap dynamically increases
 u = unk padding (offset), x = orig img contents
 Note that this padding has nothing to do with the extra canvas size allocated in mcv_mapimg... The reason for large mcv_mapimg is to standardize the full map size (perhaps for DL tranning)
 while ROI_OFFSET ensures FFP search begins from an UNKNOWN cell. That is FFP scans on the ROI (including the ROI_OFFSET region) of mcv_mapimg
*********************************************************************************************************************************************/

// u u u u u u u u
// u x x x x x x u
// u x x x x x x u
// u x x x x x x u
// u u u u u u u u

ROS_INFO("roi info: <%d %d %d %d> \n", roi.x, roi.y, roi.width, roi.height) ;

	uint8_t ukn = static_cast<uchar>(dffp::MapStatus::UNKNOWN) ;
	cv::Mat img_plus_offset = cv::Mat( img_.rows + ROI_OFFSET*2, img_.cols + ROI_OFFSET*2, CV_8U, cv::Scalar(ukn) ) ;
	cv::Rect myroi( ROI_OFFSET, ROI_OFFSET, img_.cols, img_.rows );
	cv::Mat img_roi = img_plus_offset(myroi) ;
	img_.copyTo(img_roi) ;

	cv::Mat processed_gmap = mcvu_mapimg.clone() ;
	if( mn_numpyrdownsample > 0)
	{
		// be careful here... using pyrDown() interpolates occ and free, making the boarder area (0 and 127) to be 127/2 !!
		// 127 reprents an occupied cell !!!
		for(int iter=0; iter < mn_numpyrdownsample; iter++ )
		{
			int nrows = mcvu_mapimg.rows; //% 2 == 0 ? img.rows : img.rows + 1 ;
			int ncols = mcvu_mapimg.cols; // % 2 == 0 ? img.cols : img.cols + 1 ;
			//ROS_INFO("sizes orig: %d %d ds: %d %d \n", img_.rows, img_.cols, nrows/2, ncols/2 );
			pyrDown(processed_gmap, processed_gmap, cv::Size( ncols/2, nrows/2 ) );
		}
	}
	clusterToThreeLabels( processed_gmap );

	geometry_msgs::PoseStamped start = GetCurrRobotPose( );

//cv::imwrite("/home/hankm/results/neuro_exploration_res/gmap_pad.png", mcvu_mapimg);
//cv::imwrite("/home/hankm/results/neuro_exploration_res/processed_gmap.png", processed_gmap);

//	dffp::FrontPropagation oFP(img_plus_offset); // image uchar
//	oFP.update(img_plus_offset, cv::Point(ngmx,ngmy), cv::Point(0,0) );
//	oFP.extractFrontierRegion( img_plus_offset ) ;
//	cv::Mat img_frontiers_offset = oFP.GetFrontierContour() ;

// DNN FFP detection
ROS_INFO("begin DNN FR detection \n");

	cv::Mat fr_img = processed_gmap.clone();

ros::WallTime GFFPstartTime = ros::WallTime::now();
	run_tf_fr_detector_session(processed_gmap, fr_img);
ros::WallTime GFFPendTime = ros::WallTime::now();
double ffp_time = (GFFPendTime - GFFPstartTime ).toNSec() * 1e-6;

//cv::imwrite("/home/hankm/results/neuro_exploration_res/fr_out.png", fr_img);

// get robot pose in the shifted gm image coordinate
	cv::Point rpos_gm = world2gridmap( cv::Point2f( start.pose.position.x, start.pose.position.y ) ) ; 	// rpose in orig gm
	rpos_gm = cv::Point( (rpos_gm.x + roi.x) / mn_scale, (rpos_gm.y + roi.y) / mn_scale ) ;  			// rpos in padded img --> rpos in ds img

//cv::Mat tmp;
//cv::cvtColor( processed_gmap, tmp, cv::COLOR_GRAY2BGR);
//cv::circle(tmp, rpos_gm,  10, cv::Scalar(255,0,0), 1, 8, 0 );
//cv::imwrite("/home/hankm/results/neuro_exploration_res/out_rpose.png", tmp);

	cv::Mat gmap_tform, fr_img_tform, astar_net_input ;

	m_data_handler.transform_map_to_robotposition(fr_img, rpos_gm.x, rpos_gm.y, 0, fr_img_tform) ;  // tform fr_img
	m_data_handler.transform_map_to_robotposition(processed_gmap, rpos_gm.x, rpos_gm.y, 127, gmap_tform) ;  // tform fr_img
	cv::Mat gaussimg_32f = m_data_handler.GetGaussianImg();
	m_data_handler.generate_astar_net_input(fr_img_tform, gmap_tform, gaussimg_32f, astar_net_input);
//cv::Mat bgr[3] ;
//split(astar_net_input, bgr) ;
//cv::Mat I0,I1,I2, G ;
//bgr[0].convertTo(I0, CV_8U, 255.f) ;
//bgr[1].convertTo(I1, CV_8U, 255.f) ;
//bgr[2].convertTo(I2, CV_8U, 255.f) ;
//gaussimg_32f.convertTo(G, CV_8U, 255.f);
//cv::imwrite("/home/hankm/results/neuro_exploration_res/img0.png", I0);
//cv::imwrite("/home/hankm/results/neuro_exploration_res/img1.png", I1);
//cv::imwrite("/home/hankm/results/neuro_exploration_res/img2.png", I2);
//cv::imwrite("/home/hankm/results/neuro_exploration_res/gauss.png", G);

//cv::imwrite("/home/hankm/results/neuro_exploration_res/gmap_tfrom.png", gmap_tform);
//cv::imwrite("/home/hankm/results/neuro_exploration_res/gauss_img.png", gaussimg_32f * 255.f);
//cv::imwrite("/home/hankm/results/neuro_exploration_res/astar_net_input.png", astar_net_input * 255.f);

//////////////////////////////////////////////////////////////////////////////////////////////
// 						run astar prediction
//////////////////////////////////////////////////////////////////////////////////////////////
ros::WallTime GPstartTime = ros::WallTime::now();
	cv::Mat potmap_prediction ;
	run_tf_astar_session( astar_net_input,  potmap_prediction) ;

ros::WallTime GPendTime = ros::WallTime::now();
double planning_time = (GPendTime - GPstartTime ).toNSec() * 1e-6;

//cv::imwrite("/home/hankm/results/neuro_exploration_res/potmap_prediction.png", potmap_prediction);

	// select an optimal frontier point
	vector<cv::Point> optpt_cands;
    locate_optimal_point_from_potmap( potmap_prediction, mn_num_classes-1, optpt_cands ) ;

    vector<PointClass> potmap_points, potmap_points_tformed ;
    assign_potmap_point_class( potmap_prediction, potmap_points);
	// tform the opt frontier points
	m_data_handler.inv_transform_point_to_robotposition( potmap_points, processed_gmap.rows, processed_gmap.cols,
			 	 	 	 	 	 	 	 	 	 	 	 rpos_gm.x, rpos_gm.y, potmap_points_tformed); //optpt_cands_tformed ) ;

	// The last tform to original coordinate
	int xoffset = roi.x / mn_scale ; // roi is defined in max_global_height (1024)
	int yoffset = roi.y / mn_scale ;
ROS_INFO("eliminating roi_offset <%d %d> \n", xoffset, yoffset);
	for( int jj=0; jj < potmap_points_tformed.size(); jj++)
	{
		potmap_points_tformed[jj].x = potmap_points_tformed[jj].x - xoffset ;
		potmap_points_tformed[jj].y = potmap_points_tformed[jj].y - yoffset ;
	}

    vector<cv::Point> optpt_cands_tformed ;
	for(int ii=0; ii < potmap_points_tformed.size(); ii++)
	{
		if(potmap_points_tformed[ii].label == mn_num_classes-1)
		{
			optpt_cands_tformed.push_back( cv::Point( potmap_points_tformed[ii].x, potmap_points_tformed[ii].y) );
		}
	}

//	PointClassSet potmap_point_class( rgb(0.f, 0.f, 1.f), rgb(1.f, 1.f, 0.f), mn_num_classes ) ;
//	for( int jj=0; jj < optpt_cands_tformed.size(); jj++)
//	{
//		int label =
//		potmap_point_class.push_point(pc)
//	}


//cv::Mat im_tform_gray, im_tform;
//im_tform_gray = cv::Mat::zeros(potmap_prediction.rows, potmap_prediction.cols, CV_8U);
//for(int idx=0; idx < optpt_cands_tformed.size(); idx++)
//{
//	int lidx = optpt_cands_tformed[idx].x + processed_gmap.cols * optpt_cands_tformed[idx].y ;
//	im_tform_gray.data[lidx] = 255;
//}
//cv::cvtColor(im_tform_gray, im_tform, cv::COLOR_GRAY2BGR);
//cv::circle(im_tform, rpos_gm,  10, cv::Scalar(255,0,0), 1, 8, 0 );
//cv::imwrite("/home/hankm/results/neuro_exploration_res/tformed_back.png", im_tform);

	// tranform potmap pixels back to the (non-center) robot's position

//ROS_ASSERT(0);

ROS_INFO("img_frontiers_offset (DNN output) size: %d %d \n", fr_img.rows, fr_img.cols );
// locate the most closest labeled points w.r.t the centroid pts

	vector<vector<cv::Point> > model_output_contours; //, contours_plus_offset;
	vector<cv::Vec4i> hierarchy;
	cv::findContours( fr_img, model_output_contours, hierarchy, CV_RETR_EXTERNAL, CV_CHAIN_APPROX_NONE );

#ifdef FD_DEBUG_MODE
	string outfilename =  m_str_debugpath + "/global_mapimg.png" ;
//	cv::imwrite( outfilename.c_str(), m_uMapImg);
//	cv::imwrite(m_str_debugpath + "/labeled_img.png", img);
//	cv::imwrite(m_str_debugpath + "/img_frontiers.png",img_frontiers);
#endif

//////////////////////////////////////////////////////////////////////////////////////////////
//////////////// 		update unreachable frontiers;
//////////////////////////////////////////////////////////////////////////////////////////////
	updateUnreachablePointSet( globalcostmap );
	// init fr viz markers
	visualization_msgs::Marker vizfrontier_regions;
	vizfrontier_regions = SetVizMarker( 0, visualization_msgs::Marker::ADD, 0.f, 0.f, 0.f, m_worldFrameId,	1.f, 0.f, 0.f, 0.1);
	vizfrontier_regions.type = visualization_msgs::Marker::POINTS;

	visualization_msgs::Marker vizminpot_regions; // min astar distance regions
	vizfrontier_regions = SetVizMarker( 0, visualization_msgs::Marker::ADD, 0.f, 0.f, 0.f, m_worldFrameId,	0.f, 1.f, 1.f, 0.1);
	vizfrontier_regions.type = visualization_msgs::Marker::POINTS;

	// init curr frontier point sets
	{
		const std::unique_lock<mutex> lock_prev(mutex_prev_frontier_set);
		const std::unique_lock<mutex> lock_curr(mutex_curr_frontier_set);
		const std::unique_lock<mutex> lock_unrc(mutex_unreachable_frontier_set);
		// append valid previous frontier points after the sanity check
		for (const auto & pi : m_prev_frontier_set)
		{
//ROS_INFO("checking reachability %f %f \n", pi.p[0], pi.p[1] );
			int ngmx = static_cast<int>( (pi.p[0] - globalcostmap.info.origin.position.x) / cmresolution ) ;
			int ngmy = static_cast<int>( (pi.p[1] - globalcostmap.info.origin.position.y) / cmresolution ) ;
			if( frontier_sanity_check(ngmx, ngmy, cmwidth, cmdata) ) //if( cmdata[ ngmy * cmwidth + ngmx ] == -1 )
			{
				if( mo_frontierfilter.isReachable( m_unreachable_frontier_set, pi.p[0], pi.p[1] ) )
				{
					pointset pnew( pi.p[0], pi.p[1] );
					m_curr_frontier_set.insert( pnew );
				}
			}
		}
	}
// ROS_INFO("done updating vizfrontiers \n");

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Note that model_output_contours are found in DNN output img in which contains extra padding ( using roi ) to make fixed size: 512 512 img.
// THe actual contours <contours_plus_offset> must be the ones found in actual gridmap (dynamic sized), thus we must remove the roi offset !!
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

//	int xoffset = roi.x / mn_scale ; // roi is defined in max_global_height (1024)
//	int yoffset = roi.y / mn_scale ;
	vector<vector<cv::Point> > contours_plus_offset = model_output_contours;
//	for( int ii=0; ii < contours_plus_offset.size(); ii++ )
//	{
//		for( int jj=0; jj < contours_plus_offset[ii].size(); jj++)
//		{
//			contours_plus_offset[ii][jj].x = model_output_contours[ii][jj].x - xoffset ;
//			contours_plus_offset[ii][jj].y = model_output_contours[ii][jj].y - yoffset ;
//
//			FrontierPoint oFRpoint( cv::Point(contours_plus_offset[ii][jj].x - ROI_OFFSET, contours_plus_offset[ii][jj].y - ROI_OFFSET), cmheight, cmwidth, m_gridmap.info.origin.position.y, m_gridmap.info.origin.position.x,
//								   gmresolution, mn_numpyrdownsample );
//
//			geometry_msgs::Point point_w;
//			point_w.x = oFRpoint.GetInitWorldPosition().x ;
//			point_w.y = oFRpoint.GetInitWorldPosition().y ;
//			vizfrontier_regions.points.push_back( point_w ) ;
//		}
//	}


	for( int ii=0; ii < potmap_points_tformed.size(); ii++ )
	{
		if (potmap_points_tformed[ii].label < mn_num_classes - 1)
			continue;

		FrontierPoint oFRpoint( cv::Point(potmap_points_tformed[ii].x - ROI_OFFSET, potmap_points_tformed[ii].y - ROI_OFFSET), cmheight, cmwidth,
								m_gridmap.info.origin.position.y, m_gridmap.info.origin.position.x, gmresolution, mn_numpyrdownsample );
		geometry_msgs::Point point_w;
		point_w.x = oFRpoint.GetInitWorldPosition().x ;
		point_w.y = oFRpoint.GetInitWorldPosition().y ;
		vizfrontier_regions.points.push_back( point_w ) ;
	}


	if( contours_plus_offset.size() == 0 )
	{
		ROS_WARN("There is no more frontier region left in this map \n");

		// we update/refresh the frontier points viz markers and publish them...
		// we don't need to proceed to planning
	}
	else // We found fr, update append new points to m_curr_frontier_set accordingly
	{
		vector<FrontierPoint> voFrontierCands;

		for( int i = 0; i < optpt_cands_tformed.size(); i++ )
		{
			cv::Point frontier ; // frontier points found in the down-sampled map image.
			frontier.x = optpt_cands_tformed[i].x - ROI_OFFSET ;  // optpt is defined in DS image, so we need to upsample the point back
			frontier.y = optpt_cands_tformed[i].y - ROI_OFFSET ;
			FrontierPoint oPoint( frontier, cmheight, cmwidth,
									m_gridmap.info.origin.position.y, m_gridmap.info.origin.position.x,
						   gmresolution, mn_numpyrdownsample );

	/////////////////////////////////////////////////////////////////////
	// 				We need to run position correction here
	/////////////////////////////////////////////////////////////////////
			cv::Point init_pt 		= oPoint.GetInitGridmapPosition() ; 	// position @ ds0 (original sized map)
			cv::Point corrected_pt	= oPoint.GetCorrectedGridmapPosition() ;
			correctFrontierPosition( m_gridmap, init_pt, mn_correctionwindow_width, corrected_pt  );
			oPoint.SetCorrectedCoordinate(corrected_pt); // frontiers in up-scaled (orig img coord) image
			voFrontierCands.push_back(oPoint);
		}

		// run filter
		const float fcm_conf = mo_frontierfilter.GetCostmapConf() ;
		const float fgm_conf = mo_frontierfilter.GetGridmapConf() ;

		// eliminate frontier points at obtacles

		if( globalcostmap.info.width > 0 )
		{
ROS_ERROR("getting error here in conf measure fn b/c costmap size is downsampled \n");
			mo_frontierfilter.measureCostmapConfidence(globalcostmap, voFrontierCands);
ROS_INFO("done CM filter \n");
			mo_frontierfilter.measureGridmapConfidence(m_gridmap, voFrontierCands);
ROS_INFO("done GM filter \n");

			for(size_t idx=0; idx < voFrontierCands.size(); idx++)
			{
				cv::Point frontier_in_gm = voFrontierCands[idx].GetCorrectedGridmapPosition();
				bool bisfrontier = is_frontier_point(frontier_in_gm.x, frontier_in_gm.y, cmwidth, cmheight, gmdata );
				int gmidx = cmwidth * frontier_in_gm.y	+	frontier_in_gm.x ;
				bool bisexplored = cmdata[gmidx] >=0 ? true : false ;
				voFrontierCands[idx].SetFrontierFlag( fcm_conf, fgm_conf, bisexplored, bisfrontier );

//ROS_INFO("%f %f %d %d\n", voFrontierCands[idx].GetInitWorldPosition().x, voFrontierCands[idx].GetInitWorldPosition().y,
//						  voFrontierCands[idx].GetCorrectedGridmapPosition().x, voFrontierCands[idx].GetCorrectedGridmapPosition().y);

			}
			set<pointset> unreachable_frontiers;
			{
				const std::unique_lock<mutex> lock_unrc(mutex_unreachable_frontier_set) ;
				unreachable_frontiers = m_unreachable_frontier_set ;
			}
			mo_frontierfilter.computeReachability( unreachable_frontiers, voFrontierCands );
		}
		else
		{
			//ROS_INFO("costmap hasn't updated \n");
			//frontiers = frontiers_cand ; // points in img coord
		}

		{
			//vector<size_t> valid_frontier_indexs;
			const std::unique_lock<mutex> lock(mutex_curr_frontier_set);
			for (size_t idx=0; idx < voFrontierCands.size(); idx++)
			{
				if( voFrontierCands[idx].isConfidentFrontierPoint() )
				{
					//valid_frontier_indexs.push_back( idx );
					cv::Point2f frontier_in_world = voFrontierCands[idx].GetCorrectedWorldPosition();
					pointset pt( frontier_in_world.x, frontier_in_world.y );
					m_curr_frontier_set.insert( pt );
				}
			}
		}
	}

// print frontier list
//ROS_INFO("\n ****************\n **** frontier list **** \n **************************\n");
//for( const auto & pi : m_curr_frontier_set)
//{
//	geometry_msgs::PoseStamped tmp_goal = StampedPosefromSE2( pi.p[0], pi.p[1], 0.f );
//	cv::Point gmpt = world2gridmap( cv::Point2f(pi.p[0], pi.p[1]) );
//	ROS_INFO("%f %f %d %d\n", pi.p[0], pi.p[1], gmpt.x, gmpt.y );
//}

// save frontier info ;
//ROS_INFO(" The num of tot frontier points left :  %d\n", m_curr_frontier_set.size() );
	//frontier_summary( voFrontierCands );

	//publishFrontierPointMarkers( ) ;
	publishFrontierRegionMarkers ( vizfrontier_regions );

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// 		generate a path trajectory
// 		call make plan service
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//ROS_INFO(" FR is found let's start the make plan service \n");

///////////////////////////////////////////////////////////////////////
// 1. estimate dist to each goal using euclidean distance heuristic (we need sorting here)
///////////////////////////////////////////////////////////////////////
	float fstartx = static_cast<float>( start.pose.position.x ) ;
	float fstarty = static_cast<float>( start.pose.position.y ) ;

//////////////////////////////////////////////////////////////////////////////////
// 2. use the fp corresponds to the min distance as the init fp. epsilon = A*(fp)
// 	i)  We first sort fpts based on their euc heuristic(), then try makePlan() for each of fpts in turn.
// 	ii) We need to sort them b/c the one with best heuristic could fail
//////////////////////////////////////////////////////////////////////////////////

	geometry_msgs::PoseStamped goal;
	nav_msgs::Path msg_frontierpoints ;
	for( const auto & pi : m_curr_frontier_set)
	{
		geometry_msgs::PoseStamped tmp_goal = StampedPosefromSE2( pi.p[0], pi.p[1], 0.f );
		tmp_goal.header.frame_id = m_worldFrameId ;

		float fdist_sq = (start.pose.position.x - tmp_goal.pose.position.x ) * ( start.pose.position.y - tmp_goal.pose.position.y ) ;
		float fdist = sqrt(fdist_sq);
		if (fdist > 1.f)
			msg_frontierpoints.poses.push_back(tmp_goal);
	}

	if( m_curr_frontier_set.empty() || msg_frontierpoints.poses.size() == 0 ) // terminating condition
	{
		ROS_WARN("no more valid frontiers \n");
		// delete markers
		visualization_msgs::MarkerArray ftmarkers_old = m_frontierpoint_markers ;
		for(size_t idx=0; idx < ftmarkers_old.markers.size(); idx++)
			ftmarkers_old.markers[idx].action = visualization_msgs::Marker::DELETE; //SetVizMarker( idx, visualization_msgs::Marker::DELETE, 0.f, 0.f, 0.5, "map", 0.f, 1.f, 0.f );
		m_markerfrontierPub.publish(ftmarkers_old);

		ftmarkers_old = m_unreachable_markers ;
		for(size_t idx=0; idx < ftmarkers_old.markers.size(); idx++)
			ftmarkers_old.markers[idx].action = visualization_msgs::Marker::DELETE; //SetVizMarker( idx, visualization_msgs::Marker::DELETE, 0.f, 0.f, 0.5, "map", 0.f, 1.f, 0.f );
		m_marker_unreachpointPub.publish(ftmarkers_old);

		mb_explorationisdone = true;
		return;
	}

ROS_INFO(" got valid frontier points \n");


	// publish goalexclusive fpts
	int tmpcnt = 0;
	nav_msgs::Path goalexclusivefpts ;
	goalexclusivefpts.header.frame_id = m_worldFrameId;
	goalexclusivefpts.header.seq = m_curr_frontier_set.size() -1 ;
	goalexclusivefpts.header.stamp = ros::Time::now();
	for (const auto & pi : m_curr_frontier_set)
	{
		if (tmpcnt != 0)
		{
			geometry_msgs::PoseStamped fpt = StampedPosefromSE2( pi.p[0], pi.p[1], 0.f ) ;
			fpt.header.frame_id = m_worldFrameId ;
			fpt.header.stamp = goalexclusivefpts.header.stamp ;
			goalexclusivefpts.poses.push_back(fpt);
		}
		tmpcnt++;
	}

//ROS_INFO(" done here 1 \n");

//cv::Point2f cvbestgoal = cvgoalcands[best_idx] ;  // just for now... we need to fix it later
//	geometry_msgs::PoseStamped best_goal = msg_frontierpoints.poses[best_idx] ; //ps.p[0], ps.p[1], 0.f );

// if the best frontier point is the same as the previous frontier point, we need to set a different goal
ROS_INFO(" msg_frontierpoint size: %d \n", msg_frontierpoints.poses.size() );
	geometry_msgs::PoseStamped best_goal = msg_frontierpoints.poses[0] ; //ps.p[0], ps.p[1], 0.f );

	// check for ocsillation
	float fdist2prevposition = euc_dist( cv::Point2f( m_previous_robot_pose.pose.position.x, m_previous_robot_pose.pose.position.y ), cv::Point2f( start.pose.position.x, start.pose.position.y ) ) ;
	if( fdist2prevposition > 0.5 ) // 0.5 is ros nav stack default
	{
		m_last_oscillation_reset = ros::Time::now();
	}
//ROS_INFO(" done here 2 \n");

	bool bisocillating = (m_last_oscillation_reset + ros::Duration(8.0) < ros::Time::now() );
	if( bisocillating ) // first check for oscillation
	{
		ROS_WARN("Oscillation detected. Set <%f %f> as unreachable fpt \n", m_previous_goal.pose.pose.position.x, m_previous_goal.pose.pose.position.y);

//		publishGoalPointMarker( ) ;

		// publish current goal as unreachable pt
		geometry_msgs::PoseStamped ufpt = StampedPosefromSE2( m_previous_goal.pose.pose.position.x, m_previous_goal.pose.pose.position.y, 0.f ) ;
		appendUnreachablePoint(  ufpt ) ;
		publishUnreachableMarkers( );

		updatePrevFrontierPointsList( ) ;

		// update m_prev_frontier_set / m_curr_frontier_set list
		mb_strict_unreachable_decision = true;
		m_last_oscillation_reset = ros::Time::now();
		return ;
	}
	else if( equals_to_prevgoal( best_goal ) ) //|| me_prev_exploration_state == ABORTED ) // cond to select the next best alternative
	{
		ROS_WARN(" The best target <%f %f> found by the planner is the same as the previous goal. Need to select an alternative target. \n",
				best_goal.pose.position.x,  best_goal.pose.position.y );

		mb_nbv_selected = true ;
		if( goalexclusivefpts.poses.size() == 0 )
		{
			ROS_WARN(" However, there is no more frontier points to visit. \n");
			mb_explorationisdone = true;
			return;
		}
//		else if(goalexclusivefpts.poses.size() <=  mn_prev_nbv_posidx + 1 ) // we have
//		{
//			ROS_WARN(" We have tried every alternative goals... retrying from the beginning.. \n");
//			mn_prev_nbv_posidx = 0;
//			return;
//		}

		geometry_msgs::PoseStamped ufpt = StampedPosefromSE2( best_goal.pose.position.x, best_goal.pose.position.y, 0.f ) ;
		appendUnreachablePoint(  ufpt ) ;

//		ROS_ASSERT( goalexclusivefpts.poses.size() >  mn_prev_nbv_posidx );

		// choose the next best goal based on the eucdist heurisitic.
ROS_WARN("The target goal is equal to the previous goal... Selecting NBV point from <%d goalexclusivefpts> to be the next best target \n", goalexclusivefpts.poses.size() );
		geometry_msgs::PoseStamped nextbestpoint = StampedPosefromSE2( 0.f, 0.f, 0.f ) ;
		selectNextBestPoint( start,  goalexclusivefpts, nextbestpoint) ;
ROS_WARN("Selecting the next best point since frontier pts is unreachable ..  \n");
		const std::unique_lock<mutex> lock(mutex_currgoal);
		m_targetgoal.header.frame_id = m_worldFrameId ;
		m_targetgoal.pose.pose = nextbestpoint.pose ;
		m_previous_goal = m_targetgoal ;
	}
	else // ordinary case ( choosing the optimal pt, then move toward there )
	{
		//mn_prev_nbv_posidx = -1;
		mb_nbv_selected = false ;
		const std::unique_lock<mutex> lock(mutex_currgoal);
		m_targetgoal.header.frame_id = m_worldFrameId ;
		m_targetgoal.pose.pose = best_goal.pose ;
		m_previous_goal = m_targetgoal ;
	}

//ROS_INFO(" done here 2 \n");

//////////////////////////////////////////////////////////////////////////////
// 						Publish frontier pts to Rviz						//
//////////////////////////////////////////////////////////////////////////////
	{
		const std::unique_lock<mutex> lock(mutex_robot_state) ;
		me_robotstate = ROBOT_STATE::ROBOT_IS_READY_TO_MOVE;
	}
//ROS_INFO(" done here 3 \n");
	updatePrevFrontierPointsList( ) ;
//ROS_INFO(" done here 4 \n");

////////////////////////////////////////////////////////////////////////////////////////////////////
// Lastly we publish the goal and other frontier points ( hands over the control to move_base )
////////////////////////////////////////////////////////////////////////////////////////////////////
	m_previous_robot_pose = start;
//	m_otherfrontierptsPub.publish(goalexclusivefpts);
	publishUnreachableMarkers( );
	m_currentgoalPub.publish(m_targetgoal);		// for control

ros::WallTime mapCallEndTime = ros::WallTime::now();
double mapcallback_time = (mapCallEndTime - mapCallStartTime).toNSec() * 1e-6;
ROS_DEBUG("\n "
		 " ************************************************************************* \n "
		 "	 \t mapDataCallback exec time (ms): %f ( %f planning time) \n "
		 " ************************************************************************* \n "
		, mapcallback_time, planning_time);

ROS_INFO("********** \t End of mapdata callback routine \t ********** \n");

	//saveDNNData( img_frontiers_offset, start, best_goal, best_plan, ROI_OFFSET, roi ) ;

// for timing
mf_totalcallbacktime_msec = mf_totalcallbacktime_msec + mapcallback_time ;
mf_totalplanningtime_msec = mf_totalplanningtime_msec + planning_time ;
mf_totalffptime_msec	  = mf_totalffptime_msec	  + ffp_time ;

mn_mapcallcnt++;
}

void FrontierDetectorDMS::run_tf_fr_detector_session( const cv::Mat& input_map, cv::Mat& model_output)
{
	memset( mpf_fd_data, 0.f, input_map.cols * input_map.rows*sizeof(float) );
	memset( mpf_fd_result, 0.f, input_map.cols * input_map.rows*sizeof(float) );

    for(int ridx=0; ridx < input_map.rows; ridx++)
    {
    	for(int cidx=0; cidx < input_map.cols; cidx++)
    	{
    		int lidx = ridx * input_map.cols + cidx ;
    		mpf_fd_data[lidx] = static_cast<float>(input_map.data[lidx]) / 255.f   ;
    	}
    }
    const int ndata = sizeof(float)*1*input_map.rows*input_map.cols*1 ;
    const int64_t dims[] = {1, input_map.rows, input_map.cols, 1};
    TF_Tensor* int_tensor = TF_NewTensor(TF_FLOAT, dims, 4, mpf_fd_data, ndata, &NoOpDeallocator, 0);
    if (int_tensor != NULL)
    {
    	ROS_INFO("TF_NewTensor is OK\n");
    }
    else
    	ROS_INFO("ERROR: Failed TF_NewTensor\n");

    mpptf_fd_input_values[0] = int_tensor;
    // //Run the Session

ros::WallTime SessionStartTime = ros::WallTime::now();
    TF_SessionRun(mptf_fd_Session, NULL, mptf_fd_input, mpptf_fd_input_values, 1, mptf_fd_output, mpptf_fd_output_values, 1, NULL, 0, NULL, mptf_fd_Status);
ros::WallTime SessionEndTime = ros::WallTime::now();
double session_time = (SessionEndTime - SessionStartTime).toNSec() * 1e-6;
mf_total_fd_sessiontime_msec = mf_total_fd_sessiontime_msec + session_time ;

    if(TF_GetCode(mptf_fd_Status) == TF_OK)
    {
    	ROS_INFO("Session is OK\n");
    }
    else
    {
    	ROS_INFO("%s",TF_Message(mptf_fd_Status));
    }

    cv::Mat res_32F(512, 512, CV_32FC1, TF_TensorData(*mpptf_fd_output_values));
	double minVal, maxVal;
	cv::minMaxLoc( res_32F, &minVal, &maxVal );
//	printf("max min: %f %f\n", maxVal, minVal);
	model_output = ( res_32F > maxVal * 0.3 ) * 255 ;
}

void FrontierDetectorDMS::run_tf_astar_session( const cv::Mat& input_map, cv::Mat& model_output)
{
	int nheight = input_map.rows ;
	int nwidth  = input_map.cols ;
	int nchannels = input_map.channels() ;
	int nclasses = mn_num_classes ;

	memset( mpf_astar_data, 0.f, nheight * nwidth * nchannels * sizeof(float) );
//	memset( mpf_astar_result, 0.f, nheight * nwidth * nchannels * sizeof(float) );

	// C H W style { i.e.) depth first then shift to right }
	for(int ridx=0; ridx < nheight; ridx++)
	{
		for(int cidx=0; cidx < nwidth; cidx++)
		{
			//int lidx = ridx * 512 * 3 + cidx * NUM_CLASSES + chidx ;
			//int lidx = ridx * 512 + cidx + 512 * 512 * chidx ;
			float c0 = input_map.at<cv::Vec3f>(ridx, cidx)[0]   ;
			float c1 = input_map.at<cv::Vec3f>(ridx, cidx)[1]   ;
			float c2 = input_map.at<cv::Vec3f>(ridx, cidx)[2]   ;

			int idx0 = ridx * nheight * nchannels + cidx * nchannels + 0  ;
			int idx1 = ridx * nheight * nchannels + cidx * nchannels + 1  ;
			int idx2 = ridx * nheight * nchannels + cidx * nchannels + 2 ;
			mpf_astar_data[idx0] = c0 ;
			mpf_astar_data[idx1] = c1 ;
			mpf_astar_data[idx2] = c2 ;
		}
	}

    const int ndata = sizeof(float)*1*input_map.rows * input_map.cols * input_map.channels() ;
    const int64_t dims[] = {1, input_map.rows, input_map.cols, input_map.channels()};
    TF_Tensor* int_tensor = TF_NewTensor(TF_FLOAT, dims, 4, mpf_astar_data, ndata, &NoOpDeallocator, 0);
    if (int_tensor != NULL)
    {
    	ROS_INFO("TF_NewTensor is OK\n");
    }
    else
    	ROS_INFO("ERROR: Failed TF_NewTensor\n");

    mpptf_astar_input_values[0] = int_tensor;
    // //Run the Session
ros::WallTime SessionStartTime = ros::WallTime::now();
    TF_SessionRun(mptf_astar_Session, NULL, mptf_astar_input, mpptf_astar_input_values, 1, mptf_astar_output, mpptf_astar_output_values, 1, NULL, 0, NULL, mptf_astar_Status);
ros::WallTime SessionEndTime = ros::WallTime::now();
double session_time = (SessionEndTime - SessionStartTime).toNSec() * 1e-6;
mf_total_astar_sessiontime_msec += session_time ;

    if(TF_GetCode(mptf_astar_Status) == TF_OK)
    {
    	ROS_INFO("Session is OK\n");
    }
    else
    {
    	ROS_INFO("%s",TF_Message(mptf_astar_Status));
    }

	void* buff = TF_TensorData(mpptf_astar_output_values[0]);
	float* offsets = (float*)buff;

	model_output = cv::Mat::zeros(nheight, nwidth, CV_8U);

	// C H W style
	for (int ridx = 0; ridx < nheight; ++ridx)
	{
		for (int cidx = 0; cidx < nwidth; ++cidx)
		{
			int max_idx = -1;
			float max_val = -FLT_MAX;

			for(int chidx=0; chidx < nclasses; chidx++)	// hard coded for now...
			{
				int idx = ridx * nheight * nclasses + cidx * nclasses + chidx ;
				float val = offsets[idx] ;
				if (val > max_val)
				{
					max_val = val;
					max_idx = chidx;
				}
				//tmp.at<float>(ridx, cidx) = val ;
			}
			model_output.data[ridx*nheight + cidx] = max_idx ;
		}
	}
}

int FrontierDetectorDMS::locate_optimal_point_from_potmap( const cv::Mat& input_potmap, const uint8_t& optVal, vector<cv::Point>& points   )
{
	//cv::Mat optFR = input_potmap == optVal ;
	int nheight = input_potmap.rows ;
	int nwidth = input_potmap.cols ;
	int ncnt = 0;
	for( int ii = 0; ii < nheight; ii++ )
	{
		for(int jj =0; jj < nwidth; jj++)
		{
			int idx = nwidth * ii + jj ;
			if( input_potmap.data[ idx ] == optVal )
			{
				points.push_back(cv::Point(jj,ii));
				ncnt++;
			}
		}
	}
	return ncnt ;
}

int FrontierDetectorDMS::assign_potmap_point_class( const cv::Mat& input_potmap, vector<PointClass>& points   )
{
	//cv::Mat optFR = input_potmap == optVal ;
	int nheight = input_potmap.rows ;
	int nwidth = input_potmap.cols ;
	int ncnt = 0;
	for( int ii = 0; ii < nheight; ii++ )
	{
		for(int jj =0; jj < nwidth; jj++)
		{
			int idx = nwidth * ii + jj ;
			uint8_t label = input_potmap.data[ idx ];
			points.push_back( PointClass(jj, ii, (int)label) );
			ncnt++;
		}
	}
	return ncnt ;
}



void FrontierDetectorDMS::saveDNNData( const cv::Mat& img_frontiers_offset, const geometry_msgs::PoseStamped& start, const geometry_msgs::PoseStamped& best_goal,
								const std::vector<geometry_msgs::PoseStamped>& best_plan, const int& OFFSET, const cv::Rect& roi )
{
//	string strgmapimg 	= (boost::format("%s/mapimg%05d.png") % m_str_debugpath.c_str() % mn_mapcallcnt ).str() ;
//	string strmetadata 	= (boost::format("%s/metadata%05d.txt") % m_str_debugpath.c_str() % mn_mapcallcnt ).str() ;
//	string strimgFR		= (boost::format("%s/imgfr%05.txt") %  m_str_debugpath.c_str() % mn_mapcallcnt ).str() ;

	char cgmapimg[300], ccmimg[300], cmetadata[300], cimgFR[300], cbestplan[300];
	sprintf(cgmapimg,	"%s/gmimg%04d.png", m_str_debugpath.c_str(),mn_mapcallcnt);
	sprintf(ccmimg, 	"%s/cmimg%04d.png", m_str_debugpath.c_str(), mn_mapcallcnt );
	sprintf(cmetadata,	"%s/metadata%04d.txt", m_str_debugpath.c_str(),mn_mapcallcnt);
	sprintf(cimgFR,		"%s/frimg%04d.png", m_str_debugpath.c_str(),mn_mapcallcnt);
	sprintf(cbestplan,	"%s/bestplan%04d.txt", m_str_debugpath.c_str(), mn_mapcallcnt);

	// save gridmap and costmap
	unsigned char* pmap = mpo_costmap->getCharMap() ;
	cv::Mat cvcmapimg = cv::Mat( mcvu_mapimg.rows, mcvu_mapimg.cols, CV_8U);
	cv::imwrite( string(cgmapimg), mcvu_mapimg ) ;
	cv::imwrite( string(ccmimg), mcvu_costmapimg) ;
	// save metadata (robot pose, opt frontier point, )
	std::ofstream ofs_metadata( cmetadata );
	ofs_metadata <<
			start.pose.position.x << " " << start.pose.position.y << " " <<
			start.pose.orientation.x << " " <<	start.pose.orientation.y << " " << start.pose.orientation.z << " " << start.pose.orientation.w << " " <<
			best_goal.pose.position.x << " " << best_goal.pose.position.y << " " <<
			m_gridmap.info.height << " " << m_gridmap.info.width << " " << m_gridmap.info.origin.position.x << " " << m_gridmap.info.origin.position.y << " " << m_gridmap.info.resolution << " " <<
			OFFSET << " " << roi.x << " " << roi.y << " " << roi.height << " " << roi.width << endl;

	ofs_metadata.close();
	// save frontier region
	cv::imwrite( string(cimgFR), img_frontiers_offset );

	std::ofstream ofs_bestplan( cbestplan );
	for (int nidx=0; nidx < best_plan.size(); nidx++)
		ofs_bestplan << best_plan[nidx].pose.position.x << " " << best_plan[nidx].pose.position.y << endl;
	ofs_bestplan.close();
}


void FrontierDetectorDMS::doneCB( const actionlib::SimpleClientGoalState& state )
{
//    ROS_INFO("@DONECB: simpleClientGoalState [%s]", state.toString().c_str());

    if (m_move_client.getState() == actionlib::SimpleClientGoalState::SUCCEEDED)
    {
         // do something as goal was reached
    	ROS_INFO("Touch down  \n");
		{
			const std::unique_lock<mutex> lock(mutex_robot_state) ;
			me_robotstate = ROBOT_STATE::ROBOT_IS_NOT_MOVING ;
		}
		me_prev_exploration_state = SUCCEEDED ;
    }
    else if (m_move_client.getState() == actionlib::SimpleClientGoalState::ABORTED)
    {
        // do something as goal was canceled
    	ROS_ERROR("MoveBase() has failed to reach the goal. The frontier point has aboarted ... \n");
		{
			const std::unique_lock<mutex> lock(mutex_robot_state) ;
			me_robotstate = ROBOT_STATE::ROBOT_IS_NOT_MOVING ;
		}
		me_prev_exploration_state = ABORTED ;
    }
    else
    {
    	ROS_ERROR("doneCB() received an unknown state %d \n", m_move_client.getState());
    	exit(-1);
    }
}


void FrontierDetectorDMS::moveRobotCallback(const geometry_msgs::PoseWithCovarianceStamped::ConstPtr& msg )
{
// call actionlib
// robot is ready to move
	ROS_INFO("@moveRobotCallback Robot is < %s > \n ",  robot_state[me_robotstate+1] );

	if( me_robotstate >= ROBOT_STATE::FORCE_TO_STOP   )
		return;

	geometry_msgs::PoseWithCovarianceStamped goalpose = *msg ;
	{
		const std::unique_lock<mutex> lock(mutex_robot_state) ;
		me_robotstate = ROBOT_STATE::ROBOT_IS_MOVING ;
	}

	ROS_INFO("@moveRobotCallback received a plan\n");

	move_base_msgs::MoveBaseGoal goal;
	goal.target_pose.header.frame_id = m_worldFrameId; //m_baseFrameId ;
	goal.target_pose.header.stamp = ros::Time::now() ;

//	geometry_msgs::PoseWithCovarianceStamped goalpose = // m_pathplan.poses.back() ;

	goal.target_pose.pose.position.x = goalpose.pose.pose.position.x ;
	goal.target_pose.pose.position.y = goalpose.pose.pose.position.y ;
	goal.target_pose.pose.orientation.w = goalpose.pose.pose.orientation.w ;

	ROS_INFO("new destination target is set to <%f  %f> \n", goal.target_pose.pose.position.x, goal.target_pose.pose.position.y );

	publishUnreachableMarkers( ) ;

	// publish goal to Rviz
	if( !mb_nbv_selected )
	{
		publishGoalPointMarker( goalpose );
	}
	else
	{
//		m_targetgoal_marker.points.clear();
//		m_targetgoal_marker = SetVizMarker( -1, visualization_msgs::Marker::ADD, m_targetgoal.pose.pose.position.x, m_targetgoal.pose.pose.position.y, 0.f,
//				m_worldFrameId,	0.58f, 0.44f, 0.86f, (float)TARGET_MARKER_SIZE);
//		m_makergoalPub.publish(m_targetgoal_marker); // for viz
		publishGoalPointMarker( goalpose );
		mb_nbv_selected = false ;
	}

// inspect the path
//////////////////////////////////////////////////////////////////////////////////////////////
//ROS_INFO("+++++++++++++++++++++++++ @moveRobotCallback, sending a goal +++++++++++++++++++++++++++++++++++++\n");
ros::WallTime moveRobotStartTime = ros::WallTime::now();
	m_move_client.sendGoal(goal, boost::bind(&FrontierDetectorDMS::doneCB, this, _1), SimpleMoveBaseClient::SimpleActiveCallback() ) ;
//ROS_INFO("+++++++++++++++++++++++++ @moveRobotCallback, a goal is sent +++++++++++++++++++++++++++++++++++++\n");
	m_move_client.waitForResult();

ros::WallTime moveRobotEndTime = ros::WallTime::now();
double moverobot_time = (moveRobotEndTime - moveRobotStartTime).toNSec() * 1e-6;
mf_totalmotiontime_msec = mf_totalmotiontime_msec + moverobot_time ;
mn_moverobotcnt++;
}

void FrontierDetectorDMS::unreachablefrontierCallback(const geometry_msgs::PoseStamped::ConstPtr& msg )
{
	ROS_INFO("@unreachablefrontierCallback: The robot is at [%s] state\n ",  robot_state[me_robotstate+1] );

	geometry_msgs::PoseStamped unreachablepose = *msg ;

	appendUnreachablePoint( unreachablepose );

//	for (const auto & di : m_unreachable_frontier_set)
//	    ROS_WARN("unreachable pts: %f %f\n", di.d[0], di.d[1]);

	// stop the robot and restart frontier detection procedure

	ROS_WARN("+++++++++++Canceling the unreachable goal +++++++++++++\n");

	{
		const std::unique_lock<mutex> lock(mutex_robot_state) ;
		me_robotstate = ROBOT_STATE::FORCE_TO_STOP ;
	}

	m_move_client.stopTrackingGoal();
	m_move_client.waitForResult();
	m_move_client.cancelGoal();
	m_move_client.waitForResult();

	ROS_INFO("+++++++++++ Robot is ready for motion +++++++++++++\n");
	{
		const std::unique_lock<mutex> lock(mutex_robot_state) ;
		me_robotstate = ROBOT_STATE::ROBOT_IS_READY_TO_MOVE ;
	}

}


}

