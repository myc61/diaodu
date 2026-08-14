BEGIN;

SET search_path TO dispatch, public;

-- Make yaw half-angle conversion explicit and expose per-goal tolerances.
UPDATE capability_definitions
SET
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
                    "orientation":{
                        "x":0,
                        "y":0,
                        "z":"${yaw_half_sin}",
                        "w":"${yaw_half_cos}"
                    }
                },
                "distance_tolerance":"${distance_tolerance}",
                "heading_tolerance":"${heading_tolerance}"
            }],
            "translation":{"enable":false,"heading":0}
        }
    }'::jsonb,
    protocol_config = protocol_config || '{
        "default_distance_tolerance":0.15,
        "default_heading_tolerance":0.2
    }'::jsonb,
    updated_at = now()
WHERE capability_key = 'navigation'
  AND endpoint_name = '/zj_humanoid/navigation/navigation';

COMMIT;
