BEGIN;

SET search_path TO dispatch, public;

-- Keep workflow/command history after a robot is deleted. In-progress runs are
-- still rejected in application code before this DELETE fires.

ALTER TABLE node_runs
    DROP CONSTRAINT IF EXISTS node_runs_assigned_robot_id_fkey;

ALTER TABLE node_runs
    ADD CONSTRAINT node_runs_assigned_robot_id_fkey
    FOREIGN KEY (assigned_robot_id) REFERENCES robots(id) ON DELETE SET NULL;

ALTER TABLE command_runs
    DROP CONSTRAINT IF EXISTS command_runs_robot_id_fkey;

ALTER TABLE command_runs
    ALTER COLUMN robot_id DROP NOT NULL;

ALTER TABLE command_runs
    ADD CONSTRAINT command_runs_robot_id_fkey
    FOREIGN KEY (robot_id) REFERENCES robots(id) ON DELETE SET NULL;

COMMIT;
