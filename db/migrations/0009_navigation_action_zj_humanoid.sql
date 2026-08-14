BEGIN;

SET search_path TO dispatch, public;

-- Align navigation capability with robot-side navigation/NavigationAction.
UPDATE capability_definitions
SET
    ros_message_type = 'navigation/NavigationAction',
    parameter_schema = '{
        "type":"object",
        "required":["x","y","yaw","distance_tolerance","heading_tolerance"],
        "properties":{
            "x":{"type":"number"},
            "y":{"type":"number"},
            "yaw":{"type":"number"},
            "distance_tolerance":{"type":"number","exclusiveMinimum":0,"default":0.15},
            "heading_tolerance":{"type":"number","exclusiveMinimum":0,"default":0.2}
        }
    }'::jsonb,
    request_template = '{
        "header":{"frame_id":"map"},
        "goal_id":{"id":"${command_id}"},
        "goal":{
            "header":{"frame_id":"map"},
            "task_type":{"value":0},
            "waypoints":[{
                "pose":{
                    "position":{"x":"${x}","y":"${y}","z":0},
                    "orientation":{"x":0,"y":0,"z":"${yaw_half_sin}","w":"${yaw_half_cos}"}
                },
                "distance_tolerance":"${distance_tolerance}",
                "heading_tolerance":"${heading_tolerance}"
            }],
            "translation":{"enable":false,"heading":0}
        }
    }'::jsonb,
    protocol_config = '{
        "adapter":"ROS1_ACTIONLIB",
        "goal_topic":"/zj_humanoid/navigation/navigation/goal",
        "cancel_topic":"/zj_humanoid/navigation/navigation/cancel",
        "status_topic":"/zj_humanoid/navigation/navigation/status",
        "feedback_topic":"/zj_humanoid/navigation/navigation/feedback",
        "result_topic":"/zj_humanoid/navigation/navigation/result",
        "default_distance_tolerance":0.15,
        "default_heading_tolerance":0.2
    }'::jsonb,
    updated_at = now()
WHERE capability_key = 'navigation'
  AND (
      ros_message_type IS DISTINCT FROM 'navigation/NavigationAction'
      OR endpoint_name = '/zj_humanoid/navigation/navigation'
  );

COMMIT;
