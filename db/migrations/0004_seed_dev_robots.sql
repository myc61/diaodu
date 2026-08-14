BEGIN;

SET search_path TO dispatch, public;

INSERT INTO capability_profiles (id, name, description)
VALUES (
    '11111111-1111-1111-1111-111111111111',
    'zj_humanoid_noetic',
    'Development profile for zj_humanoid Noetic robots'
)
ON CONFLICT (name) DO NOTHING;

INSERT INTO robots (
    id,
    name,
    enabled,
    robot_type,
    ros_version,
    capability_profile_id,
    business_status,
    localization_status,
    metadata
)
VALUES
    (
        '22222222-2222-2222-2222-222222222201',
        'robot_a',
        true,
        'zj_humanoid',
        'ROS1',
        '11111111-1111-1111-1111-111111111111',
        'IDLE',
        'UNKNOWN',
        '{"label":"Robot A"}'::jsonb
    ),
    (
        '22222222-2222-2222-2222-222222222202',
        'robot_b',
        true,
        'zj_humanoid',
        'ROS1',
        '11111111-1111-1111-1111-111111111111',
        'IDLE',
        'UNKNOWN',
        '{"label":"Robot B"}'::jsonb
    )
ON CONFLICT (name) DO NOTHING;

INSERT INTO robot_connections (
    robot_id,
    rosbridge_url,
    host,
    rosbridge_port,
    rosbridge_path,
    rosbridge_tls,
    ros_namespace,
    pose_topic,
    pose_message_type,
    pose_mapping,
    stale_timeout_ms,
    state,
    configuration_state,
    ros_distribution
)
VALUES
    (
        '22222222-2222-2222-2222-222222222201',
        'ws://127.0.0.1:9090/',
        '127.0.0.1',
        9090,
        '/',
        false,
        '',
        '/zj_humanoid/navigation/odom_info',
        'nav_msgs/Odometry',
        '{
            "x_path":"pose.pose.position.x",
            "y_path":"pose.pose.position.y",
            "quaternion_x_path":"pose.pose.orientation.x",
            "quaternion_y_path":"pose.pose.orientation.y",
            "quaternion_z_path":"pose.pose.orientation.z",
            "quaternion_w_path":"pose.pose.orientation.w",
            "frame_id_path":"header.frame_id",
            "timestamp_path":"header.stamp"
        }'::jsonb,
        3000,
        'DISCONNECTED',
        'READY',
        'noetic'
    ),
    (
        '22222222-2222-2222-2222-222222222202',
        'ws://127.0.0.1:9091/',
        '127.0.0.1',
        9091,
        '/',
        false,
        '',
        '/zj_humanoid/navigation/odom_info',
        'nav_msgs/Odometry',
        '{
            "x_path":"pose.pose.position.x",
            "y_path":"pose.pose.position.y",
            "quaternion_x_path":"pose.pose.orientation.x",
            "quaternion_y_path":"pose.pose.orientation.y",
            "quaternion_z_path":"pose.pose.orientation.z",
            "quaternion_w_path":"pose.pose.orientation.w",
            "frame_id_path":"header.frame_id",
            "timestamp_path":"header.stamp"
        }'::jsonb,
        3000,
        'DISCONNECTED',
        'READY',
        'noetic'
    )
ON CONFLICT (robot_id) DO NOTHING;

INSERT INTO capability_definitions (
    profile_id,
    capability_key,
    operation_kind,
    endpoint_name,
    ros_message_type,
    parameter_schema,
    request_template,
    timeout_ms,
    blocking_type,
    resource_claims,
    protocol_config
)
VALUES (
    '11111111-1111-1111-1111-111111111111',
    'navigation',
    'ACTION',
    '/zj_humanoid/navigation/navigation',
    'navigation/NavigationAction',
    '{"type":"object","required":["x","y","yaw","distance_tolerance","heading_tolerance"],"properties":{"x":{"type":"number"},"y":{"type":"number"},"yaw":{"type":"number"},"distance_tolerance":{"type":"number","exclusiveMinimum":0,"default":0.15},"heading_tolerance":{"type":"number","exclusiveMinimum":0,"default":0.2}}}'::jsonb,
    '{"header":{"frame_id":"map"},"goal_id":{"id":"${command_id}"},"goal":{"header":{"frame_id":"map"},"task_type":{"value":0},"waypoints":[{"pose":{"position":{"x":"${x}","y":"${y}","z":0},"orientation":{"x":0,"y":0,"z":"${yaw_half_sin}","w":"${yaw_half_cos}"}},"distance_tolerance":"${distance_tolerance}","heading_tolerance":"${heading_tolerance}"}],"translation":{"enable":false,"heading":0}}}'::jsonb,
    120000,
    'NAVIGATION',
    '[{"name":"navigation","access":"EXCLUSIVE"}]'::jsonb,
    '{
        "adapter":"ROS1_ACTIONLIB",
        "goal_topic":"/zj_humanoid/navigation/navigation/goal",
        "cancel_topic":"/zj_humanoid/navigation/navigation/cancel",
        "status_topic":"/zj_humanoid/navigation/navigation/status",
        "feedback_topic":"/zj_humanoid/navigation/navigation/feedback",
        "result_topic":"/zj_humanoid/navigation/navigation/result",
        "default_distance_tolerance":0.15,
        "default_heading_tolerance":0.2
    }'::jsonb
)
ON CONFLICT (profile_id, capability_key) DO NOTHING;

COMMIT;
