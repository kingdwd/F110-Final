#include "trajectory_planner.hpp"


using namespace std;
namespace fs = experimental::filesystem;
// FIXME: change lookahead to var
TrajectoryPlanner::TrajectoryPlanner(ros::NodeHandle &nh) : distance_from_switch_(0)
{

    int horizon;
    std::string car = ros::this_node::getName();
    nh.getParam(car + "/horizon", horizon);
    nh.getParam(car + "/num_traj", num_traj_);
    nh.getParam(car + "/MAX_HORIZON", max_horizon_);
    nh.getParam(car + "/close_weight", close_weight);
    nh.getParam(car + "/cmaes_lookahead_1", lookahead_1_);
    nh.getParam(car + "/cmaes_lookahead_2", lookahead_2_);
    nh.getParam(car + "/switch_distance_threshold", switch_distance_threshold_);

    std::string lane_file;
    std::string lane_name;
    int lane_number = 0;

    while (true)
    {
        lane_name = "/lane_" + std::to_string(lane_number);
        lane_number++;
        if (nh.getParam(car + lane_name, lane_file))
        {
            Trajectory temporary_trajectory;
            temporary_trajectory.ReadCMAES(lane_file);
            lanes_.push_back(temporary_trajectory);
        }
        else
        {
            break;
        }
    }
    horizon_ = horizon;
    std::string vis_topic;
    nh.getParam(car + "/trajectory_vis", vis_topic);
    traj_pub_ = nh.advertise<visualization_msgs::Marker>(vis_topic, 1);
    ROS_INFO("planner created");
}

TrajectoryPlanner::~TrajectoryPlanner()
{
    ROS_INFO("killing the planner");
}

void TrajectoryPlanner::ReadTrajectories()
{
    string path = ros::package::getPath("milestone-3")+"/csv/local_traj_50.csv";
    cout << path << endl;
    ifstream input(path);
    string coordX, coordY;
    if (input.is_open())
    {
        while (getline(input,coordX,','))
        {
            getline(input,coordY);
            trajectories_.push_back(pair<float,float>(stof(coordY),stof(coordX)));
        }
        cout<<"got trajectories \n";
        cout<<trajectories_.size();
        // each trajectory has 10 pairs of points, total 100 pairs are present for 10 trajectories
    }
    else
    {
        cout << "Please run this from the root catkin_ws directory" << endl;
        exit(0);
    }
}


pair<float,float> TrajectoryPlanner::CarPoint2World(float x, float y, const geometry_msgs::Pose &current_pose)
{
    tf2::Transform car_to_world;
    geometry_msgs::Transform car_to_world_msg;
    geometry_msgs::TransformStamped car_to_world_stamped;
    tf2::fromMsg(current_pose, car_to_world);
    car_to_world_msg = tf2::toMsg(car_to_world);
    car_to_world_stamped.transform = car_to_world_msg;
    geometry_msgs::Vector3 carPoint;
    geometry_msgs::Vector3 worldPoint;
    carPoint.x = x;
    carPoint.y = y;
    carPoint.z = 0;
    tf2::doTransform(carPoint, worldPoint, car_to_world_stamped);
    float carPoseX = current_pose.position.x;
    float carPoseY = current_pose.position.y;
    return pair<float,float>(worldPoint.x+carPoseX, worldPoint.y+carPoseY);
}


void TrajectoryPlanner::Trajectory2world(const geometry_msgs::Pose &current_pose)
{
    trajectories_world.clear();
    for (unsigned int i=0; i<trajectories_.size(); i++)
    {
        trajectories_world.push_back(CarPoint2World(trajectories_[i].first, trajectories_[i].second,current_pose));
    }
    // ROS_INFO("trajectories in world frame");
}

int TrajectoryPlanner::BestTrajectory(OccGrid &occ_grid, const geometry_msgs::Pose &current_pose)
{
    float max_distance = -std::numeric_limits<float>::max();
    int furtherest_index = -1;
    int best = -1;
    pair<float,float> closest_cmaes;
    pair<float,float> car_pose(current_pose.position.x,current_pose.position.y);
    for (int ii= 0; ii<num_traj_; ii++)
    {
        bool collision = true;

        for (int l=0; l<horizon_ - 1; l++)
        {
            collision = occ_grid.CheckCollision(trajectories_world[max_horizon_*ii+l],trajectories_world[max_horizon_*ii+l+1]);
            if (!collision)
            {
                //cout<<ii<<" collision"<<endl;
                break;
            }
            //cout << l << ",";
        }

        // 0 1.44218

        pair<pair<float,float>,int> tp = lanes_[current_lane_].FindClosest(car_pose);
        int ind_car = tp.second;
        if (collision)
        {
            // cout<<ii<<" no_collision"<<endl;
            pair<float,float> end_point = trajectories_world[max_horizon_*ii + horizon_-1];
            pair<pair<float,float>,int> ans = lanes_[current_lane_].FindClosest(end_point);

            pair <float,float> temp = ans.first;
            int ind = ans.second;
            float dist1 = Transforms::CalcDist(end_point,temp);
            float dist2 = Transforms::CalcDist(temp,car_pose);
            collision = occ_grid.CheckCollision(end_point,temp);
            float eff_dist = dist2-close_weight*dist1;
            bool check;
            if (ind_car>40)
            {
                check = false;
            }
            if (eff_dist>max_distance && collision)
            {
                if (check)
                {
                    if (ind>(furtherest_index-1))
                    {
                        max_distance = eff_dist;
                        closest_cmaes = temp;
                        best = ii;
                        furtherest_index = ind;
                    }
                }

                else
                {
                    max_distance = eff_dist;
                    closest_cmaes = temp;
                    best = ii;
                    furtherest_index = ind;
                }


            }
        }

    }
    if (!cmaes_point_pushed_)
    {
        geometry_msgs::Point curr_point;
        curr_point.x = closest_cmaes.first;
        curr_point.y = closest_cmaes.second;
        curr_point.z = 0.2;
        points_.push_back(curr_point);
        std_msgs::ColorRGBA curr_color;//(1.0, 0.0, 1.0, 1.0);
        curr_color.r = 0;
        curr_color.g = 0;
        curr_color.b = 1;
        curr_color.a = 1;
        colors_.push_back(curr_color);
        cmaes_point_pushed_ = true;
    }
    State push;
    push.set_x(trajectories_world[max_horizon_ * best].first);
    push.set_y(trajectories_world[max_horizon_ * best].second);
    double dx = (trajectories_world[max_horizon_ * best + 1].first) - (trajectories_world[max_horizon_ * best].first);
    double dy = (trajectories_world[max_horizon_ * best + 1].second) - (trajectories_world[max_horizon_ * best].second);
    double ori = atan2(dy, dx);
    push.set_ori(ori);
    best_minipath_.clear();
    best_minipath_.push_back(push);
    for (int ii =  1; ii < horizon_; ++ii)
    {
        push.set_x(trajectories_world[max_horizon_ * best + ii].first);
        push.set_y(trajectories_world[max_horizon_ * best + ii].second);
        dx = (trajectories_world[max_horizon_ * best + ii].first) - (trajectories_world[max_horizon_ * best + ii - 1].first);
        dy = (trajectories_world[max_horizon_ * best + ii].second) - (trajectories_world[max_horizon_ * best + ii - 1].second);
        ori = atan2(dy, dx);
        push.set_ori(ori);
        best_minipath_.push_back(push);
    }
    best_cmaes_point_.set_x(trajectories_world[max_horizon_ * best + horizon_-1].first);
    best_cmaes_point_.set_y(trajectories_world[max_horizon_ * best + horizon_-1].second);
    dx = (trajectories_world[max_horizon_ * best + horizon_-1].first) - (trajectories_world[max_horizon_ * best + horizon_ - 2].first);
    dy = (trajectories_world[max_horizon_ * best + horizon_-1].second) - (trajectories_world[max_horizon_ * best + horizon_ - 2].second);
    ori = atan2(dy, dx);
    // cout << dx << "\t" << dy << "\t" << ori << endl;
    best_cmaes_point_.set_ori(ori);
    // cout<<best<<"is the best"<<endl;
    return best;
}

void TrajectoryPlanner::SelectLane(const geometry_msgs::Pose pose, OccGrid &occ_grid)
{
    unsigned int old_lane = current_lane_;
    for (unsigned int lane = 0; lane < lanes_.size(); ++lane)
    {
        if (lane != current_lane_ && lanes_[lane].IsPathCollisionFree(pose, occ_grid, lookahead_1_, lookahead_2_))
        {
            current_lane_ = lane;
            if (old_lane < current_lane_)
            {
                distance_from_switch_ = 0;
            }
            break;
        }
    }
    // std::cout << distance_from_switch_ << std::endl;
}

vector<State> TrajectoryPlanner::best_minipath()
{
    return best_minipath_;
}

int TrajectoryPlanner::best_trajectory_index()
{
    return best_trajectory_index_;
}

void TrajectoryPlanner::Visualize()
{
    std::vector<pair<float,float>> best_traj;
    for (int i = max_horizon_*best_trajectory_index_; i<max_horizon_*best_trajectory_index_+horizon_; i++)
    {
        best_traj.push_back(trajectories_world[i]);
    }
    std::vector<geometry_msgs::Point> best_traj_points = Visualizer::GenerateVizPoints(best_traj);
    std::vector<std_msgs::ColorRGBA> best_traj_colors = Visualizer::GenerateVizColors(best_traj, 0, 1, 0);
    points_.insert(points_.end(), best_traj_points.begin(), best_traj_points.end());
    colors_.insert(colors_.end(), best_traj_colors.begin(), best_traj_colors.end());
    best_traj_pushed_ = true;
    // best_traj_pub_.publish(Visualizer::GenerateSphereList(best_traj, 0, 1, 0));
    auto pts = lanes_[current_lane_].GetPairPoints();
    std::vector<geometry_msgs::Point> cmaes_points = Visualizer::GenerateVizPoints(pts);
    std::vector<std_msgs::ColorRGBA> cmaes_colors = Visualizer::GenerateVizColors(pts, 1, 0, 0);
    points_.insert(points_.end(), cmaes_points.begin(), cmaes_points.end());
    colors_.insert(colors_.end(), cmaes_colors.begin(), cmaes_colors.end());
    cmaes_pushed_ = true;
    traj_pub_.publish(Visualizer::GenerateList(points_, colors_));
    cmaes_point_pushed_ = false;
    best_traj_pushed_ = false;
    cmaes_pushed_ = false;
    points_.clear();
    colors_.clear();
    // visualizeCmaes();
}
void TrajectoryPlanner::Update(const geometry_msgs::Pose &current_pose, OccGrid &occ_grid)
{
    //trajectory2miniworld(current_pose);
    distance_from_switch_ += Transforms::CalcDist(std::pair<float,float>(last_pose_.position.x, last_pose_.position.y), std::pair<float,float>(current_pose.position.x, current_pose.position.y));
    Trajectory2world(current_pose);

    if (distance_from_switch_ > switch_distance_threshold_ || !lanes_[current_lane_].IsPathCollisionFree(current_pose, occ_grid, lookahead_1_, lookahead_2_))
    {
        SelectLane(current_pose, occ_grid);
    }
    best_trajectory_index_ = BestTrajectory(occ_grid, current_pose);
    last_pose_ = current_pose;
}
