BEGIN;

SET search_path TO dispatch, public;

-- Align zj_humanoid pose subscription with robot-side odom_info (nav_msgs/Odometry).
UPDATE robot_connections
SET
    pose_topic = '/zj_humanoid/navigation/odom_info',
    pose_message_type = 'nav_msgs/Odometry',
    pose_mapping = '{
        "x_path":"pose.pose.position.x",
        "y_path":"pose.pose.position.y",
        "quaternion_x_path":"pose.pose.orientation.x",
        "quaternion_y_path":"pose.pose.orientation.y",
        "quaternion_z_path":"pose.pose.orientation.z",
        "quaternion_w_path":"pose.pose.orientation.w",
        "frame_id_path":"header.frame_id",
        "timestamp_path":"header.stamp"
    }'::jsonb
WHERE robot_id IN (
    SELECT id FROM robots WHERE robot_type = 'zj_humanoid'
)
AND (
    pose_topic IS NULL
    OR pose_topic = ''
    OR pose_topic = '/amcl_pose'
);

COMMIT;
