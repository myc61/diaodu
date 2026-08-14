BEGIN;

CREATE EXTENSION IF NOT EXISTS pgcrypto;
CREATE SCHEMA IF NOT EXISTS dispatch;

SET search_path TO dispatch, public;

CREATE TABLE capability_profiles (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    name text NOT NULL UNIQUE,
    description text NOT NULL DEFAULT '',
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE scenes (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    name text NOT NULL UNIQUE,
    description text NOT NULL DEFAULT '',
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE map_versions (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    scene_id uuid NOT NULL REFERENCES scenes(id) ON DELETE CASCADE,
    version integer NOT NULL CHECK (version > 0),
    status text NOT NULL DEFAULT 'READY'
        CHECK (status IN ('IMPORTING', 'READY', 'FAILED', 'ARCHIVED')),
    pgm_path text NOT NULL,
    yaml_path text NOT NULL,
    preview_path text NOT NULL,
    sha256 text NOT NULL,
    width integer NOT NULL CHECK (width > 0),
    height integer NOT NULL CHECK (height > 0),
    resolution double precision NOT NULL CHECK (resolution > 0),
    origin_x double precision NOT NULL,
    origin_y double precision NOT NULL,
    origin_yaw double precision NOT NULL,
    frame_id text NOT NULL DEFAULT 'map',
    source_type text NOT NULL
        CHECK (source_type IN ('UPLOAD', 'ROBOT_PULL')),
    source_metadata jsonb NOT NULL DEFAULT '{}'::jsonb,
    created_at timestamptz NOT NULL DEFAULT now(),
    UNIQUE (scene_id, version)
);

CREATE TABLE robots (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    name text NOT NULL UNIQUE,
    enabled boolean NOT NULL DEFAULT true,
    robot_type text NOT NULL,
    ros_version text NOT NULL CHECK (ros_version IN ('ROS1', 'ROS2')),
    capability_profile_id uuid REFERENCES capability_profiles(id),
    current_scene_id uuid REFERENCES scenes(id),
    current_map_version_id uuid REFERENCES map_versions(id),
    business_status text NOT NULL DEFAULT 'UNKNOWN',
    localization_status text NOT NULL DEFAULT 'UNKNOWN',
    last_pose jsonb,
    last_pose_at timestamptz,
    metadata jsonb NOT NULL DEFAULT '{}'::jsonb,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE robot_connections (
    robot_id uuid PRIMARY KEY REFERENCES robots(id) ON DELETE CASCADE,
    rosbridge_url text NOT NULL,
    ros_namespace text NOT NULL DEFAULT '',
    pose_topic text NOT NULL,
    pose_message_type text NOT NULL,
    pose_mapping jsonb NOT NULL,
    stale_timeout_ms integer NOT NULL DEFAULT 3000
        CHECK (stale_timeout_ms > 0),
    state text NOT NULL DEFAULT 'DISCONNECTED'
        CHECK (
            state IN (
                'DISCONNECTED',
                'CONNECTING',
                'ONLINE',
                'DEGRADED',
                'RECONNECTING',
                'DISABLED'
            )
        ),
    connected_at timestamptz,
    last_message_at timestamptz,
    updated_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE capability_definitions (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    profile_id uuid NOT NULL
        REFERENCES capability_profiles(id) ON DELETE CASCADE,
    capability_key text NOT NULL,
    operation_kind text NOT NULL
        CHECK (operation_kind IN ('TOPIC', 'SERVICE', 'ACTION', 'CONFIG')),
    endpoint_name text NOT NULL,
    ros_message_type text NOT NULL,
    parameter_schema jsonb NOT NULL DEFAULT '{}'::jsonb,
    request_template jsonb NOT NULL DEFAULT '{}'::jsonb,
    feedback_mapping jsonb NOT NULL DEFAULT '{}'::jsonb,
    result_mapping jsonb NOT NULL DEFAULT '{}'::jsonb,
    success_condition jsonb,
    failure_condition jsonb,
    timeout_ms integer NOT NULL DEFAULT 30000 CHECK (timeout_ms > 0),
    retry_policy jsonb NOT NULL DEFAULT '{}'::jsonb,
    cancel_policy jsonb NOT NULL DEFAULT '{}'::jsonb,
    blocking_type text NOT NULL DEFAULT 'NONE'
        CHECK (blocking_type IN ('HARD', 'SOFT', 'NONE', 'NAVIGATION')),
    resource_claims jsonb NOT NULL DEFAULT '[]'::jsonb,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now(),
    UNIQUE (profile_id, capability_key)
);

CREATE TABLE devices (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    name text NOT NULL UNIQUE,
    enabled boolean NOT NULL DEFAULT true,
    device_type text NOT NULL,
    protocol text NOT NULL CHECK (protocol IN ('HTTP', 'WEBSOCKET', 'MQTT')),
    connection_config jsonb NOT NULL,
    capability_config jsonb NOT NULL DEFAULT '{}'::jsonb,
    state text NOT NULL DEFAULT 'DISCONNECTED',
    last_message_at timestamptz,
    metadata jsonb NOT NULL DEFAULT '{}'::jsonb,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE stations (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    scene_id uuid NOT NULL REFERENCES scenes(id) ON DELETE CASCADE,
    map_version_id uuid NOT NULL REFERENCES map_versions(id) ON DELETE CASCADE,
    name text NOT NULL,
    x double precision NOT NULL,
    y double precision NOT NULL,
    yaw double precision NOT NULL,
    metadata jsonb NOT NULL DEFAULT '{}'::jsonb,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now(),
    UNIQUE (map_version_id, name)
);

CREATE TABLE workflow_definitions (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    name text NOT NULL UNIQUE,
    description text NOT NULL DEFAULT '',
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE workflow_versions (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    workflow_definition_id uuid NOT NULL
        REFERENCES workflow_definitions(id) ON DELETE CASCADE,
    version integer NOT NULL CHECK (version > 0),
    status text NOT NULL DEFAULT 'DRAFT'
        CHECK (status IN ('DRAFT', 'PUBLISHED', 'ARCHIVED')),
    graph jsonb NOT NULL,
    input_schema jsonb NOT NULL DEFAULT '{}'::jsonb,
    output_schema jsonb NOT NULL DEFAULT '{}'::jsonb,
    validation_result jsonb NOT NULL DEFAULT '{}'::jsonb,
    created_at timestamptz NOT NULL DEFAULT now(),
    published_at timestamptz,
    UNIQUE (workflow_definition_id, version)
);

CREATE TABLE workflow_runs (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    workflow_version_id uuid NOT NULL REFERENCES workflow_versions(id),
    trigger_type text NOT NULL
        CHECK (trigger_type IN ('MANUAL', 'CRON', 'ROBOT_EVENT', 'DEVICE_EVENT')),
    trigger_metadata jsonb NOT NULL DEFAULT '{}'::jsonb,
    priority integer NOT NULL DEFAULT 0,
    state text NOT NULL DEFAULT 'PENDING'
        CHECK (
            state IN (
                'PENDING',
                'RUNNING',
                'PAUSED',
                'SUCCEEDED',
                'FAILED',
                'CANCELLING',
                'CANCELLED',
                'RECOVERING'
            )
        ),
    input_data jsonb NOT NULL DEFAULT '{}'::jsonb,
    context_data jsonb NOT NULL DEFAULT '{}'::jsonb,
    started_at timestamptz,
    finished_at timestamptz,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE node_runs (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    workflow_run_id uuid NOT NULL
        REFERENCES workflow_runs(id) ON DELETE CASCADE,
    node_key text NOT NULL,
    attempt integer NOT NULL DEFAULT 1 CHECK (attempt > 0),
    state text NOT NULL DEFAULT 'PENDING'
        CHECK (
            state IN (
                'PENDING',
                'WAITING_RESOURCE',
                'RUNNING',
                'WAITING_EVENT',
                'PAUSED',
                'SUCCEEDED',
                'FAILED',
                'CANCELLING',
                'CANCELLED',
                'RECOVERING'
            )
        ),
    assigned_robot_id uuid REFERENCES robots(id),
    input_data jsonb NOT NULL DEFAULT '{}'::jsonb,
    output_data jsonb NOT NULL DEFAULT '{}'::jsonb,
    error_data jsonb,
    started_at timestamptz,
    finished_at timestamptz,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now(),
    UNIQUE (workflow_run_id, node_key, attempt)
);

CREATE TABLE workflow_events (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    workflow_run_id uuid NOT NULL
        REFERENCES workflow_runs(id) ON DELETE CASCADE,
    node_run_id uuid REFERENCES node_runs(id) ON DELETE CASCADE,
    event_type text NOT NULL,
    deduplication_key text,
    payload jsonb NOT NULL DEFAULT '{}'::jsonb,
    occurred_at timestamptz NOT NULL DEFAULT now()
);

CREATE UNIQUE INDEX workflow_events_deduplication_key_uq
    ON workflow_events (workflow_run_id, deduplication_key)
    WHERE deduplication_key IS NOT NULL;

CREATE TABLE command_outbox (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    command_id uuid NOT NULL UNIQUE,
    workflow_run_id uuid NOT NULL REFERENCES workflow_runs(id),
    node_run_id uuid NOT NULL REFERENCES node_runs(id),
    target_type text NOT NULL CHECK (target_type IN ('ROBOT', 'DEVICE')),
    target_id uuid NOT NULL,
    operation_kind text NOT NULL,
    payload jsonb NOT NULL,
    state text NOT NULL DEFAULT 'PENDING'
        CHECK (
            state IN (
                'PENDING',
                'SENDING',
                'SENT',
                'ACKNOWLEDGED',
                'FAILED',
                'UNCERTAIN',
                'CANCELLED'
            )
        ),
    attempt integer NOT NULL DEFAULT 1 CHECK (attempt > 0),
    next_attempt_at timestamptz,
    sent_at timestamptz,
    acknowledged_at timestamptz,
    last_error jsonb,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE resource_leases (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    lease_key text NOT NULL UNIQUE,
    owner_type text NOT NULL CHECK (owner_type IN ('NODE_RUN', 'SYSTEM')),
    owner_id uuid NOT NULL,
    robot_id uuid REFERENCES robots(id) ON DELETE CASCADE,
    blocking_type text NOT NULL
        CHECK (blocking_type IN ('HARD', 'SOFT', 'NONE', 'NAVIGATION')),
    claims jsonb NOT NULL,
    state text NOT NULL DEFAULT 'ACTIVE'
        CHECK (state IN ('ACTIVE', 'RELEASED', 'EXPIRED')),
    acquired_at timestamptz NOT NULL DEFAULT now(),
    released_at timestamptz
);

CREATE TABLE operation_logs (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    operation_id uuid NOT NULL UNIQUE,
    actor_id uuid,
    client_ip inet,
    session_id text,
    operation_type text NOT NULL,
    object_type text NOT NULL,
    object_id text,
    before_data jsonb,
    after_data jsonb,
    result text NOT NULL,
    error_data jsonb,
    occurred_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE config_snapshots (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    robot_id uuid NOT NULL REFERENCES robots(id) ON DELETE CASCADE,
    provider_type text NOT NULL CHECK (provider_type IN ('ROS', 'HTTP', 'SSH')),
    config_key text NOT NULL,
    checksum text NOT NULL,
    content_reference text NOT NULL,
    metadata jsonb NOT NULL DEFAULT '{}'::jsonb,
    created_at timestamptz NOT NULL DEFAULT now()
);

CREATE INDEX robots_scene_idx ON robots (current_scene_id);
CREATE INDEX robot_connections_state_idx ON robot_connections (state);
CREATE INDEX devices_state_idx ON devices (state);
CREATE INDEX workflow_runs_state_priority_idx
    ON workflow_runs (state, priority DESC, created_at);
CREATE INDEX node_runs_workflow_state_idx
    ON node_runs (workflow_run_id, state);
CREATE INDEX command_outbox_dispatch_idx
    ON command_outbox (state, next_attempt_at, created_at);
CREATE INDEX resource_leases_active_robot_idx
    ON resource_leases (robot_id)
    WHERE state = 'ACTIVE';
CREATE INDEX operation_logs_occurred_at_idx
    ON operation_logs (occurred_at DESC);

COMMIT;

