BEGIN;

SET search_path TO dispatch, public;

-- Default navigation goal tolerances: 0.04 m distance, 0.04 rad heading.

UPDATE capability_definitions
SET
  parameter_schema = jsonb_set(
    jsonb_set(
      COALESCE(parameter_schema, '{}'::jsonb),
      '{properties,distance_tolerance,default}',
      '0.04'::jsonb,
      true
    ),
    '{properties,heading_tolerance,default}',
    '0.04'::jsonb,
    true
  ),
  protocol_config = COALESCE(protocol_config, '{}'::jsonb)
    || jsonb_build_object(
         'default_distance_tolerance', 0.04,
         'default_heading_tolerance', 0.04
       ),
  updated_at = now()
WHERE capability_key = 'navigation';

COMMIT;
