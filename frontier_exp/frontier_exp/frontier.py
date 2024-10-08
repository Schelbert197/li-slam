import rclpy
from rclpy.node import Node
from nav_msgs.msg import OccupancyGrid
from geometry_msgs.msg import PoseStamped
import numpy as np
import tf2_ros
from tf2_ros import LookupException, ConnectivityException, ExtrapolationException


class FrontierExplorationNode(Node):
    def __init__(self):
        super().__init__('frontier_exploration_node')

        # Parameter to specify if we are in simulation or real robot
        self.declare_parameter('is_sim', True)
        self.is_sim = self.get_parameter(
            'is_sim').get_parameter_value().bool_value

        # Subscribe to the /map topic
        self.map_subscriber = self.create_subscription(
            OccupancyGrid,
            '/map',
            self.map_callback,
            10)

        # Publisher for goal pose
        self.goal_publisher = self.create_publisher(
            PoseStamped, '/goal_pose', 10)
        # Publisher for modified map with highlighted frontiers
        self.map_with_frontiers_pub = self.create_publisher(
            OccupancyGrid, '/map_with_frontiers', 10)

        # Store frontiers and map information
        self.frontiers = []
        self.map_data = None
        self.map_resolution = None
        self.map_origin = None
        self.map_info = None

        # tf2 buffer and listener for transforms
        self.tf_buffer = tf2_ros.Buffer()
        self.tf_listener = tf2_ros.TransformListener(self.tf_buffer, self)

    def map_callback(self, msg: OccupancyGrid):
        self.map_data = np.array(msg.data).reshape(
            (msg.info.height, msg.info.width))
        self.map_resolution = msg.info.resolution
        self.map_origin = (msg.info.origin.position.x,
                           msg.info.origin.position.y)
        self.map_info = msg.info  # Store map metadata

        # Get the robot's current position from the transform
        robot_position = self.get_robot_position()

        if robot_position:
            self.robot_position = robot_position
            # Detect frontiers, remove old ones, and publish nearest
            self.find_frontiers()
            self.cleanup_frontiers()
            self.publish_nearest_frontier()

            # After processing, publish the map with highlighted frontiers
            self.publish_map_with_frontiers()
        else:
            self.get_logger().warn("Unable to determine robot's position.")

    def get_robot_position(self):
        # Determine which transform to use based on the is_sim parameter
        base_frame = 'base_footprint' if self.is_sim else 'base_link'
        try:
            transform = self.tf_buffer.lookup_transform(
                'map', base_frame, rclpy.time.Time())
            # Extract position from the transform
            x = transform.transform.translation.x
            y = transform.transform.translation.y
            return (x, y)
        except (LookupException, ConnectivityException, ExtrapolationException) as e:
            self.get_logger().error(f"Error getting transform: {str(e)}")
            return None

    def find_frontiers(self):
        """Finds unknown cells with free neighbors and adds to list"""
        height, width = self.map_data.shape
        for y in range(height):
            for x in range(width):
                if self.map_data[y, x] == -1:  # Unknown cell
                    # Check if it's a frontier (bordering a free cell)
                    if self.has_free_neighbor(x, y):
                        self.frontiers.append((x, y))

    def has_free_neighbor(self, x, y):
        """Check neighboring cells to see if any are free (value 0)"""
        neighbors = [(x-1, y), (x+1, y), (x, y-1), (x, y+1)]
        for nx, ny in neighbors:
            if 0 <= nx < self.map_data.shape[1] and 0 <= ny < self.map_data.shape[0]:
                if self.map_data[ny, nx] == 0:
                    return True
        return False

    def cleanup_frontiers(self):
        """Remove frontiers that are no longer valid (explored or occupied)"""
        valid_frontiers = []
        for x, y in self.frontiers:
            if self.map_data[y, x] == -1 and self.has_free_neighbor(x, y):
                # Only add those which are still frontier
                valid_frontiers.append((x, y))
        self.frontiers = valid_frontiers

    def publish_nearest_frontier(self):
        if not self.frontiers:
            self.get_logger().info("No frontiers available.")
            return

        # Find the closest frontier to the robot
        nearest_frontier = min(
            self.frontiers, key=lambda f: self.distance_to_robot(f))

        # Convert frontier cell to world coordinates
        goal_pose = PoseStamped()
        goal_pose.header.frame_id = "map"
        goal_pose.pose.position.x, goal_pose.pose.position.y = self.cell_to_world(
            nearest_frontier)
        goal_pose.pose.position.z = 0.0  # Flat ground
        goal_pose.pose.orientation.w = 1.0  # No specific orientation required

        # Publish the goal
        self.goal_publisher.publish(goal_pose)
        self.get_logger().info(
            f"Publishing goal at {goal_pose.pose.position.x}, {goal_pose.pose.position.y}")

    def publish_map_with_frontiers(self):
        if self.map_data is None:
            self.get_logger().warn("No map data available to highlight frontiers.")
            return

        # Create a copy of the original map to modify
        modified_map_data = np.copy(self.map_data)

        # Set frontier cells to a specific value, e.g., 50 for frontiers (represents "orange" in Rviz color scale)
        for x, y in self.frontiers:
            if 0 <= y < modified_map_data.shape[0] and 0 <= x < modified_map_data.shape[1]:
                modified_map_data[y, x] = 50  # Example value for frontiers

        # Convert the modified map data back to a flat list (required by OccupancyGrid)
        modified_map_data_flat = modified_map_data.flatten().tolist()

        # Create and populate a new OccupancyGrid message
        modified_map = OccupancyGrid()
        modified_map.header.stamp = self.get_clock().now().to_msg()
        modified_map.header.frame_id = 'map'
        # Copy over map metadata (resolution, origin, etc.)
        modified_map.info = self.map_info
        modified_map.data = modified_map_data_flat

        # Publish the modified map with highlighted frontiers
        self.map_with_frontiers_pub.publish(modified_map)
        self.get_logger().info("Published map with highlighted frontiers.")

    def distance_to_robot(self, frontier):
        # Compute the distance from the robot to a frontier (in grid coordinates)
        fx, fy = frontier
        rx, ry = self.robot_position
        return np.hypot(fx - rx, fy - ry)

    def cell_to_world(self, cell):
        # Convert a grid cell to world coordinates
        x, y = cell
        world_x = self.map_origin[0] + (x * self.map_resolution)
        world_y = self.map_origin[1] + (y * self.map_resolution)
        return world_x, world_y


def main(args=None):
    rclpy.init(args=args)
    node = FrontierExplorationNode()
    rclpy.spin(node)
    node.destroy_node()
    rclpy.shutdown()


if __name__ == '__main__':
    main()
