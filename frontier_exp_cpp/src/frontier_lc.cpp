#include <rclcpp/rclcpp.hpp>
#include "rclcpp_action/rclcpp_action.hpp"
#include <nav_msgs/msg/occupancy_grid.hpp>
#include "nav2_msgs/action/compute_path_to_pose.hpp"
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <tf2_ros/buffer.h>
#include <tf2_ros/transform_listener.h>
#include <tf2/LinearMath/Quaternion.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.hpp>
#include <rclcpp_lifecycle/lifecycle_node.hpp>
#include <rcl_interfaces/msg/parameter_descriptor.hpp>
#include <nav2_util/lifecycle_node.hpp>
#include "lifecycle_msgs/msg/state.hpp"
#include <lifecycle_msgs/srv/get_state.hpp>
#include <cmath>
#include <vector>
#include <algorithm>


class FrontierExplorationNode : public nav2_util::LifecycleNode,
  public std::enable_shared_from_this<FrontierExplorationNode>
{
public:
  FrontierExplorationNode()
  : nav2_util::LifecycleNode("frontier_explorer"), tf_buffer_(get_clock()), tf_listener_(tf_buffer_)
  {
    RCLCPP_INFO(get_logger(), "Frontier explorer lifecycle node initialized");

    // Parameter to specify if we are in simulation or real robot
    // declare_parameter("use_sim_time", false);
    is_sim_ = get_parameter("use_sim_time").as_bool();

    // Store frontiers and map information
    declare_parameter("viewpoint_depth", 1.0);
    viewpoint_depth_ = get_parameter("viewpoint_depth").as_double();

    // Create callback group for path client
    path_client_cb_group_ = create_callback_group(rclcpp::CallbackGroupType::Reentrant);

    // Trigger configuration during node startup
    auto current_state = this->get_current_state();
    if (current_state.id() == lifecycle_msgs::msg::State::PRIMARY_STATE_UNCONFIGURED) {
      this->configure();
    }
  }

  // Lifecycle transition callbacks
  nav2_util::CallbackReturn on_configure(const rclcpp_lifecycle::State &)
  {
    RCLCPP_INFO(get_logger(), "Configuring...");
    // Subscribe to the /map topic
    map_subscriber_ = create_subscription<nav_msgs::msg::OccupancyGrid>(
      "/map", 10, std::bind(&FrontierExplorationNode::mapCallback, this, std::placeholders::_1));

    // Publisher for goal pose
    goal_publisher_ = create_publisher<geometry_msgs::msg::PoseStamped>("/goal_pose", 10);
    map_with_frontiers_pub_ = create_publisher<nav_msgs::msg::OccupancyGrid>(
      "/map_with_frontiers",
      10);
    return nav2_util::CallbackReturn::SUCCESS;
  }

  nav2_util::CallbackReturn on_activate(const rclcpp_lifecycle::State &)
  {
    RCLCPP_INFO(get_logger(), "Activating...");
    goal_publisher_->on_activate();
    map_with_frontiers_pub_->on_activate();

    // Create a path planner action client
    path_client_ = rclcpp_action::create_client<nav2_msgs::action::ComputePathToPose>(
      this, "compute_path_to_pose");

    // Wait for the action server to be available
    while (!path_client_->wait_for_action_server(std::chrono::seconds(1))) {
      // Check if ROS is still running (allowing for graceful shutdown with Ctrl+C)
      if (!rclcpp::ok()) {
        RCLCPP_INFO(this->get_logger(), "Shutting down node...");
        return nav2_util::CallbackReturn::FAILURE;
      }

      RCLCPP_INFO(this->get_logger(), "Waiting for Nav2 ComputePathToPose action server...");
    }
    return nav2_util::CallbackReturn::SUCCESS;
  }

  nav2_util::CallbackReturn on_deactivate(const rclcpp_lifecycle::State &)
  {
    RCLCPP_INFO(get_logger(), "Deactivating...");
    goal_publisher_->on_deactivate();
    map_with_frontiers_pub_->on_deactivate();
    path_client_.reset();
    return nav2_util::CallbackReturn::SUCCESS;
  }

  nav2_util::CallbackReturn on_cleanup(const rclcpp_lifecycle::State &)
  {
    RCLCPP_INFO(get_logger(), "Cleaning up...");
    map_subscriber_.reset();
    return nav2_util::CallbackReturn::SUCCESS;
  }

  nav2_util::CallbackReturn on_shutdown(const rclcpp_lifecycle::State &)
  {
    RCLCPP_INFO(get_logger(), "Shutting down...");
    return nav2_util::CallbackReturn::SUCCESS;
  }

private:
  // Subs and pubs
  rclcpp::Subscription<nav_msgs::msg::OccupancyGrid>::SharedPtr map_subscriber_;
  std::shared_ptr<rclcpp_lifecycle::LifecyclePublisher<geometry_msgs::msg::PoseStamped>>
  goal_publisher_;
  std::shared_ptr<rclcpp_lifecycle::LifecyclePublisher<nav_msgs::msg::OccupancyGrid>>
  map_with_frontiers_pub_;
  // Declare the action client and callback group
  rclcpp_action::Client<nav2_msgs::action::ComputePathToPose>::SharedPtr path_client_;
  rclcpp::CallbackGroup::SharedPtr path_client_cb_group_;

  nav_msgs::msg::OccupancyGrid map_data_;
  std::vector<std::pair<int, int>> frontiers_;
  std::pair<double, double> robot_position_;

  double viewpoint_depth_;
  bool is_sim_;
  double radius_ = 2.5;
  bool use_naive_ = false;
  bool path_valid_;

  tf2_ros::Buffer tf_buffer_;
  tf2_ros::TransformListener tf_listener_;
  rclcpp::TimerBase::SharedPtr timer_;

  void activation_callback()
  {
    // stuff
    auto current_state = this->get_current_state();

    if (current_state.id() == lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE) {
      RCLCPP_INFO(get_logger(), "Callback is present. Triggering activation.");
      this->activate();
    }
  }

  void mapCallback(const nav_msgs::msg::OccupancyGrid::SharedPtr msg)
  {
    map_data_ = *msg;

    auto current_state = this->get_current_state();

    // Get the robot's current viewpoint position from the transform
    auto robot_vp_position = getRobotViewpoint();
    if (robot_vp_position &&
      current_state.id() == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE)
    {
      findFrontiers();
      cleanupFrontiers();
      publishGoalFrontier();
      publishMapWithFrontiers();
    } else {
      RCLCPP_WARN(get_logger(), "Unable to determine robot's position.");
    }
  }

  std::optional<std::pair<double, double>> getRobotViewpoint()
  {
    std::string base_frame = is_sim_ ? "base_footprint" : "base_link";
    try {
      auto transform = tf_buffer_.lookupTransform("map", base_frame, tf2::TimePointZero);

      // Extract position and orientation from the transform
      double x = transform.transform.translation.x;
      double y = transform.transform.translation.y;
      tf2::Quaternion q;
      tf2::fromMsg(transform.transform.rotation, q);

      robot_position_ = std::make_pair(x, y);

      // Use setRPY to get yaw
      double roll, pitch, yaw;
      tf2::Matrix3x3(q).getRPY(roll, pitch, yaw);

      if (use_naive_ == false) {
        // Make VP 0 to allow DTR calculations later
        viewpoint_depth_ = 0.0;
      }
      // Adjust x and y based on the viewpoint depth
      double x_adj = x + std::cos(yaw) * viewpoint_depth_;
      double y_adj = y + std::sin(yaw) * viewpoint_depth_;

      RCLCPP_INFO(get_logger(), "Got new robot position at x,y: %f, %f", x, y);
      return std::make_optional(std::make_pair(x_adj, y_adj));
    } catch (const tf2::TransformException & ex) {
      RCLCPP_ERROR(get_logger(), "Error getting transform: %s", ex.what());
      return std::nullopt;
    }
  }

  void findFrontiers()
  {
    // Loop through the map and find frontiers
    int height = map_data_.info.height;
    int width = map_data_.info.width;
    const auto & data = map_data_.data;

    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        int idx = y * width + x;
        if (data[idx] == -1 && hasFreeNeighbor(x, y)) {
          frontiers_.emplace_back(x, y);
        }
      }
    }
    // Convert the vector to a string
    std::stringstream ss;
    ss << "Vector contents: ";
    for (const auto & pair : frontiers_) {
      ss << "(" << pair.first << ", " << pair.second << ") ";
    }
    // Print the vector contents of the frontier vector
    RCLCPP_DEBUG(get_logger(), "%s", ss.str().c_str());
  }

  bool hasFreeNeighbor(int x, int y)
  {
    int height = map_data_.info.height;
    int width = map_data_.info.width;
    const auto & data = map_data_.data;

    std::vector<std::pair<int, int>> neighbors = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
    for (const auto & [nx, ny] : neighbors) {
      if (nx >= 0 && nx < width && ny >= 0 && ny < height) {
        int idx = ny * width + nx;
        if (data[idx] == 0) {       // Free cell
          return true;
        }
      }
    }
    return false;
  }

  void cleanupFrontiers()
  {
    std::vector<std::pair<int, int>> valid_frontiers;
    const auto & data = map_data_.data;

    for (const auto & frontier : frontiers_) {
      int idx = frontier.second * map_data_.info.width + frontier.first;
      if (data[idx] == -1 &&
        hasFreeNeighbor(frontier.first, frontier.second) && !tooClose(frontier))
      {
        valid_frontiers.push_back(frontier);
      }
    }
    frontiers_ = valid_frontiers;
  }

  bool tooClose(const std::pair<int, int> & frontier)
  {
    return distanceToRobot(frontier) <= 0.25;
  }
//########33
  bool check_path(
    const geometry_msgs::msg::PoseStamped & start,
    const geometry_msgs::msg::PoseStamped & goal)
  {
    // Ensure that the node is in the active state
    if (this->get_current_state().id() != lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE) {
      RCLCPP_WARN(this->get_logger(), "Node is not active, cannot send goal to ComputePathToPose.");
      return false;
    }

    // Create the goal request
    auto goal_msg = nav2_msgs::action::ComputePathToPose::Goal();
    goal_msg.start = start;  // Start PoseStamped
    goal_msg.goal.pose = goal.pose;  // Goal PoseStamped

    // Asynchronously send the goal and setup a callback to handle the result
    auto send_goal_options =
      rclcpp_action::Client<nav2_msgs::action::ComputePathToPose>::SendGoalOptions();

    // Callback when the goal is accepted
    send_goal_options.goal_response_callback =
      [this](std::shared_ptr<rclcpp_action::ClientGoalHandle<nav2_msgs::action::ComputePathToPose>>
        goal_handle)
      {
        if (!goal_handle) {
          RCLCPP_ERROR(this->get_logger(), "Goal was rejected by ComputePathToPose.");
          return;
        }

        RCLCPP_INFO(this->get_logger(), "Goal accepted, waiting for result...");

        // Asynchronously get the result
        auto result_future = path_client_->async_get_result(goal_handle);

        result_future.wait(); // Wait for the result
        if (result_future.valid()) {
          auto result = result_future.get();
          if (rclcpp::Duration(result.result->planning_time).seconds() > 0.0) {
            RCLCPP_INFO(
              this->get_logger(), "Path found in %f seconds",
              rclcpp::Duration(result.result->planning_time).seconds());

            // Path is valid, return true here
            path_valid_ = true; // You can use a member variable to hold this
          } else {
            RCLCPP_INFO(this->get_logger(), "No valid path found.");
            path_valid_ = false;
          }
        } else {
          RCLCPP_ERROR(this->get_logger(), "Failed to get result from ComputePathToPose.");
          path_valid_ = false;
        }
      };

    // Send the goal asynchronously
    path_client_->async_send_goal(goal_msg, send_goal_options);

    // Return whether the path is valid or not
    return path_valid_; // Return the stored result
  }

//#######
// bool check_path(
//   const geometry_msgs::msg::PoseStamped & start,
//   const geometry_msgs::msg::PoseStamped & goal)
// {
//   // Ensure that the node is in the active state
//   if (this->get_current_state().id() != lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE) {
//     RCLCPP_WARN(this->get_logger(), "Node is not active, cannot send goal to ComputePathToPose.");
//     return false;
//   }

  //   // Create the goal request
  //   auto goal_msg = nav2_msgs::action::ComputePathToPose::Goal();
  //   goal_msg.start = start;    // Start PoseStamped
  //   goal_msg.goal.pose = goal.pose;    // Goal PoseStamped

  //   RCLCPP_INFO(this->get_logger(), "About to send goal");
  //   // Send goal and wait for result
  //   auto goal_handle_future = path_client_->async_send_goal(goal_msg);
  //   RCLCPP_INFO(this->get_logger(), "Goal sent");

  //   // Wait for the result
  //   if (rclcpp::spin_until_future_complete(this->get_node_base_interface(), goal_handle_future) !=
  //     rclcpp::FutureReturnCode::SUCCESS)
  //   {
  //     RCLCPP_ERROR(this->get_logger(), "Failed to send goal to ComputePathToPose.");
  //     return false;
  //   }

  //   RCLCPP_INFO(this->get_logger(), "Spinning to send goal");

  //   auto goal_handle = goal_handle_future.get();
  //   if (!goal_handle) {
  //     RCLCPP_ERROR(this->get_logger(), "Goal was rejected by ComputePathToPose.");
  //     return false;
  //   }

  //   // Get the result
  //   auto result_future = path_client_->async_get_result(goal_handle);
  //   if (rclcpp::spin_until_future_complete(this->get_node_base_interface(), result_future) !=
  //     rclcpp::FutureReturnCode::SUCCESS)
  //   {
  //     RCLCPP_ERROR(this->get_logger(), "Failed to get result from ComputePathToPose.");
  //     return false;
  //   }

  //   auto result = result_future.get();
  //   if (rclcpp::Duration(result.result->planning_time).seconds() > 0.0) {
  //     RCLCPP_INFO(
  //       this->get_logger(), "Path found in %f seconds",
  //       rclcpp::Duration(result.result->planning_time).seconds());
  //     return true;
  //   } else {
  //     RCLCPP_INFO(this->get_logger(), "No valid path found.");
  //     return false;
  //   }
  // }

  void publishGoalFrontier()
  {
    if (frontiers_.empty()) {
      RCLCPP_INFO(get_logger(), "No frontiers available.");
      return;
    }

    std::pair<int, int> goal_frontier;
    if (use_naive_ == true) {
      goal_frontier = *std::min_element(
        frontiers_.begin(), frontiers_.end(), [this](const auto & f1, const auto & f2)
        {
          return distanceToRobot(f1) < distanceToRobot(f2);
        });
    } else {
      goal_frontier = bestScoreFrontier();
    }

    geometry_msgs::msg::PoseStamped goal_pose;
    goal_pose.header.frame_id = "map";
    std::tie(goal_pose.pose.position.x, goal_pose.pose.position.y) = cellToWorld(goal_frontier);
    goal_pose.pose.position.z = 0.0;
    goal_pose.pose.orientation.w = 1.0;

    goal_publisher_->publish(goal_pose);
    RCLCPP_INFO(
      get_logger(), "Publishing goal at %f, %f", goal_pose.pose.position.x,
      goal_pose.pose.position.y);
  }

  void publishMapWithFrontiers()
  {
    auto modified_map = map_data_;

    for (const auto & frontier : frontiers_) {
      int idx = frontier.second * modified_map.info.width + frontier.first;
      modified_map.data[idx] = 50;       // Mark frontiers in the map
    }

    map_with_frontiers_pub_->publish(modified_map);
    RCLCPP_INFO(get_logger(), "Published map with highlighted frontiers.");
  }

  double distanceToRobot(const std::pair<int, int> & frontier)
  {
    auto [fx, fy] = cellToWorld(frontier);
    auto [rx, ry] = robot_position_;
    return std::hypot(fx - rx, fy - ry);
  }

  std::pair<double, double> cellToWorld(const std::pair<int, int> & cell)
  {
    double world_x = map_data_.info.origin.position.x + (cell.first * map_data_.info.resolution);
    double world_y = map_data_.info.origin.position.y + (cell.second * map_data_.info.resolution);
    return {world_x, world_y};
  }

  int countUnknownCellsWithinRadius(int index, double rad)
  {
    int unknown_count = 0;

    // Get map metadata
    int width = static_cast<int>(map_data_.info.width);
    int height = static_cast<int>(map_data_.info.height);
    double resolution = map_data_.info.resolution;

    // Calculate the center cell's row and column from the index
    int center_row = index / width;
    int center_col = index % width;

    // Determine the search range in cells based on the radius
    int range = static_cast<int>(std::round(rad / resolution));

    // Loop through the square neighborhood around the center cell
    for (int row = center_row - range; row <= center_row + range; ++row) {
      for (int col = center_col - range; col <= center_col + range; ++col) {
        // Skip cells outside the grid boundaries
        if (row < 0 || row >= height || col < 0 || col >= width) {
          continue;
        }

        // Compute the Euclidean distance from the center cell
        double dist =
          std::sqrt(std::pow(row - center_row, 2) + std::pow(col - center_col, 2)) * resolution;

        // Only consider cells within the specified radius
        if (dist <= rad) {
          // Calculate the index of the current cell in the OccupancyGrid data
          int cell_index = row * width + col;

          // Check if the cell is unknown (-1)
          if (map_data_.data[cell_index] == -1 &&
            !occluded(col, row, center_col, center_row, width, map_data_.data))
          {
            unknown_count++;
          }
        }
      }
    }

    return unknown_count;
  }

  std::pair<int, int> bestScoreFrontier()
  {
    // int best_score = 0;
    int best_frontier_idx;
    double total_entropy = calculateMapEntropy();
    double best_possible_entropy;
    RCLCPP_INFO(get_logger(), "Total Map Entropy %f", total_entropy);
    std::vector<double> entropies;

    // Loop through all frontiers and get score
    for (size_t i = 0; i < frontiers_.size(); ++i) {
      const auto & frontier = frontiers_.at(i);
      int idx = frontier.second * map_data_.info.width + frontier.first;
      int unknowns = countUnknownCellsWithinRadius(idx, radius_);

      // calculate current reduced entropy and place in list
      entropies.emplace_back(
        total_entropy - (unknowns * calculateEntropy(-1)) +
        (unknowns * calculateEntropy(0)));
    }

    auto min_iterator = std::min_element(entropies.begin(), entropies.end());
    best_possible_entropy = *min_iterator;
    best_frontier_idx = std::distance(entropies.begin(), min_iterator);

    geometry_msgs::msg::PoseStamped start_pose;
    geometry_msgs::msg::PoseStamped goal_pose;

    std::tie(
      goal_pose.pose.position.x,
      goal_pose.pose.position.y) = cellToWorld(frontiers_.at(best_frontier_idx));
    start_pose.pose.position.x = robot_position_.first;
    start_pose.pose.position.y = robot_position_.second;

    bool path_valid = check_path(start_pose, goal_pose);
    RCLCPP_INFO(this->get_logger(), "Path valid: %s", path_valid ? "true" : "false");

    RCLCPP_INFO(
      get_logger(), "Selecting frontier %d, with entropy reduction %f", best_frontier_idx,
      best_possible_entropy);
    return frontiers_.at(best_frontier_idx);
  }

  double calculateMapEntropy()
  {
    double entropy;
    for (const auto & cell : map_data_.data) {
      entropy += calculateEntropy(cell);
    }
    return entropy;
  }

  double calculateEntropy(int cell_value)
  {
    double v;
    if (cell_value == -1) {
      v = 0.5;
    } else if (cell_value == 0) {
      v = 0.01;
    } else if (cell_value == 100) {
      v = 0.99;
    }
    return -1 * ((v * log(v)) + ((1 - v) * log(1 - v)));
  }

  bool occluded(int x1, int y1, int x2, int y2, int width, const std::vector<int8_t> & map_data)
  {
    // Bresenham's line algorithm to generate points between (x1, y1) and (x2, y2)
    int dx = abs(x2 - x1);
    int dy = abs(y2 - y1);
    int sx = (x1 < x2) ? 1 : -1;
    int sy = (y1 < y2) ? 1 : -1;
    int err = dx - dy;

    while (x1 != x2 || y1 != y2) {
      // Check if the current cell is occupied (value 100 means occupied)
      int cell_index = y1 * width + x1;
      if (map_data[cell_index] == 100) {
        return true;  // There is an occupied cell between the two points
      }

      int e2 = 2 * err;
      if (e2 > -dy) {
        err -= dy;
        x1 += sx;
      }
      if (e2 < dx) {
        err += dx;
        y1 += sy;
      }
    }

    return false;  // No occupied cells found between the two points
  }

};

// #include "rclcpp_components/register_node_macro.hpp"
// RCLCPP_COMPONENTS_REGISTER_NODE(FrontierExplorationNode)
int main(int argc, char * argv[])
{
  // force flush of the stdout buffer.
  // this ensures a correct sync of all prints
  // even when executed simultaneously within the launch file.
  setvbuf(stdout, NULL, _IONBF, BUFSIZ);

  rclcpp::init(argc, argv);
  rclcpp::executors::SingleThreadedExecutor exe;
  std::shared_ptr<FrontierExplorationNode> lc_node =
    std::make_shared<FrontierExplorationNode>();
  exe.add_node(lc_node->get_node_base_interface());
  exe.spin();
  rclcpp::shutdown();

  return 0;
}
