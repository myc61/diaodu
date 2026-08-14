BEGIN;

SET search_path TO dispatch, public;

ALTER TABLE robot_connections
    ADD COLUMN host text NOT NULL DEFAULT '',
    ADD COLUMN rosbridge_port integer NOT NULL DEFAULT 9090
        CHECK (rosbridge_port BETWEEN 1 AND 65535),
    ADD COLUMN rosbridge_path text NOT NULL DEFAULT '/',
    ADD COLUMN rosbridge_tls boolean NOT NULL DEFAULT false,
    ADD COLUMN ros_distribution text NOT NULL DEFAULT 'noetic',
    ADD COLUMN configuration_state text NOT NULL DEFAULT 'DRAFT'
        CHECK (configuration_state IN ('DRAFT', 'READY', 'INVALID')),
    ADD COLUMN remote_access jsonb NOT NULL DEFAULT
        '{"protocol":"NONE","port":22}'::jsonb;

ALTER TABLE robot_connections
    ALTER COLUMN pose_topic DROP NOT NULL,
    ALTER COLUMN pose_message_type DROP NOT NULL;

ALTER TABLE capability_definitions
    ADD COLUMN protocol_config jsonb NOT NULL DEFAULT '{}'::jsonb;

COMMIT;
