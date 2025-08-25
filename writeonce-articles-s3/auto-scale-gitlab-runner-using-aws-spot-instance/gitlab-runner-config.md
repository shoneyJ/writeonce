```bash
concurrent = 5
check_interval = 0
log_level = "debug"
connection_max_age = "15m0s"
shutdown_timeout = 0

[session_server]
  session_timeout = 1800

[[runners]]
  name = "gitlab-docker"
  limit = 1
  url = "https://git.writeonce.de"
  id = 9
  token = "gitlab-runner-token"
  token_obtained_at = 2024-07-25T20:50:24Z
  token_expires_at = 0001-01-01T00:00:00Z
  executor = "docker+machine"
  environment = ["DOCKER_AUTH_CONFIG={\"auths\":{\"registry.writeonce.de\":{\"auth\":\"itsasecret\"}}}"]
  [runners.custom_build_dir]
  [runners.cache]
    Type = "s3"
    Shared = true
    MaxUploadedArchiveSize = 0
    [runners.cache.s3]
      ServerAddress = "s3.eu-central-1.amazonaws.com"
      AccessKey = $AWS_ACCESS_KEY_ID
      SecretKey = $AWS_SECRET_ACCESS_KEY
      BucketName = "writeonce-gitlab-runner-cache"
      BucketLocation = "eu-central-1"
  [runners.docker]
    tls_verify = false
    image = "alpine"
    privileged = false
    disable_entrypoint_overwrite = false
    oom_kill_disable = false
    disable_cache = true
    shm_size = 0
    network_mtu = 0
  [runners.machine]
    IdleCount = 0
    IdleScaleFactor = 0.0
    IdleCountMin = 0
    IdleTime = 1800
    MaxBuilds = 1
    MachineDriver = "amazonec2"
    MachineName = "gitlab-docker-machine-%s"
    MachineOptions = [
      "amazonec2-access-key=$AWS_ACCESS_KEY_ID",
      "amazonec2-secret-key=$AWS_SECRET_ACCESS_KEY",
      "amazonec2-region=eu-central-1",
      "amazonec2-vpc-id=updateme",
      "amazonec2-subnet-id=updateme",
      "amazonec2-zone=updateme",
      "amazonec2-use-private-address=false",
      "amazonec2-tags=runner-manager-name,gitlab-aws-autoscaler,gitlab,true,gitlab-runner-autoscale,true", "amazonec2-security-group=$AWS_SECURITY_GROUP",
      "amazonec2-instance-type=c4.large",
      "amazonec2-request-spot-instance=true",
      "amazonec2-spot-price=0.0528",
      "amazonec2-ami=ami-<imageid>"]
```