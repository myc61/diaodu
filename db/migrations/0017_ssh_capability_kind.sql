BEGIN;

SET search_path TO dispatch, public;

ALTER TABLE capability_definitions
    DROP CONSTRAINT IF EXISTS capability_definitions_operation_kind_check;
ALTER TABLE capability_definitions
    ADD CONSTRAINT capability_definitions_operation_kind_check
    CHECK (operation_kind IN ('TOPIC', 'SERVICE', 'ACTION', 'CONFIG', 'SSH'));

COMMENT ON COLUMN capability_definitions.protocol_config IS
    'Adapter configuration; CONTROLLED_SSH uses ssh_robot_id and ssh_profile_id';

COMMIT;
