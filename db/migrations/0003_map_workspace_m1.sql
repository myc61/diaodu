BEGIN;

SET search_path TO dispatch, public;

ALTER TABLE scenes
    ADD COLUMN IF NOT EXISTS active_map_version_id uuid;

ALTER TABLE map_versions
    ADD COLUMN IF NOT EXISTS occupied_thresh double precision
        NOT NULL DEFAULT 0.65
        CHECK (occupied_thresh >= 0.0 AND occupied_thresh <= 1.0),
    ADD COLUMN IF NOT EXISTS free_thresh double precision
        NOT NULL DEFAULT 0.196
        CHECK (free_thresh >= 0.0 AND free_thresh <= 1.0),
    ADD COLUMN IF NOT EXISTS file_size_bytes bigint
        NOT NULL DEFAULT 0
        CHECK (file_size_bytes >= 0),
    ADD COLUMN IF NOT EXISTS yaml_image text NOT NULL DEFAULT '';

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1
        FROM pg_constraint
        WHERE conname = 'scenes_active_map_version_id_fkey'
    ) THEN
        ALTER TABLE scenes
            ADD CONSTRAINT scenes_active_map_version_id_fkey
            FOREIGN KEY (active_map_version_id)
            REFERENCES map_versions(id)
            ON DELETE SET NULL;
    END IF;
END $$;

CREATE TABLE IF NOT EXISTS scene_robots (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    scene_id uuid NOT NULL REFERENCES scenes(id) ON DELETE CASCADE,
    robot_id uuid NOT NULL REFERENCES robots(id) ON DELETE CASCADE,
    valid_from timestamptz NOT NULL DEFAULT now(),
    valid_to timestamptz,
    CHECK (valid_to IS NULL OR valid_to > valid_from)
);

CREATE UNIQUE INDEX IF NOT EXISTS scene_robots_active_robot_uq
    ON scene_robots (robot_id)
    WHERE valid_to IS NULL;

CREATE INDEX IF NOT EXISTS scene_robots_scene_active_idx
    ON scene_robots (scene_id)
    WHERE valid_to IS NULL;

ALTER TABLE command_outbox
    ALTER COLUMN workflow_run_id DROP NOT NULL,
    ALTER COLUMN node_run_id DROP NOT NULL;

COMMIT;
