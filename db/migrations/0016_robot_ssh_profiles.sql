BEGIN;

SET search_path TO dispatch, public;

ALTER TABLE robot_startup_profiles
    ALTER COLUMN ssh_username SET DEFAULT 'naviai';

UPDATE robot_startup_profiles
SET ssh_username = 'naviai', updated_at = now()
WHERE btrim(ssh_username) = '';

COMMENT ON TABLE robot_startup_profiles IS
    'Versioned controlled SSH execution profiles for robot scripts, commands, startup, readiness and stop steps';

COMMIT;
