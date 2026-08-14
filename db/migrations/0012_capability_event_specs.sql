-- 0012: editable event specs on capabilities / station actions
-- Feedback/result field predicates are custom (not discoverable via rosapi).

SET search_path TO dispatch, public;

ALTER TABLE capability_definitions
    ADD COLUMN IF NOT EXISTS event_specs jsonb NOT NULL DEFAULT '[]'::jsonb;

ALTER TABLE station_actions
    ADD COLUMN IF NOT EXISTS event_specs jsonb NOT NULL DEFAULT '[]'::jsonb;

COMMENT ON COLUMN capability_definitions.event_specs IS
    'Array of EventSpec: event_name, source(RESULT|FEEDBACK), when{field,op,value}, max_firings, cooldown_ms';

COMMENT ON COLUMN station_actions.event_specs IS
    'Optional override of capability event_specs; empty array means inherit capability specs + success_event_name';

-- Example: RESULT success event for pick_and_place (keeps success_event_name compatible).
UPDATE capability_definitions
SET event_specs = '[
  {
    "event_name": "pick_and_place_done",
    "source": "RESULT",
    "enabled": true,
    "when": {"op": "ros_success"},
    "max_firings": 1,
    "cooldown_ms": 0,
    "emit_on_node": true,
    "start_workflows": true
  }
]'::jsonb,
    updated_at = now()
WHERE capability_key = 'pick_and_place'
  AND (event_specs IS NULL OR event_specs = '[]'::jsonb);
