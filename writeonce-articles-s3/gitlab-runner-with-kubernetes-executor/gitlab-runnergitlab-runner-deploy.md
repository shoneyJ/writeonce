```yaml
apiVersion: v1
kind: Namespace
metadata:
  name: gitlab
---
apiVersion: v1
kind: ServiceAccount
metadata:
  name: gitlab-runner
  namespace: gitlab
---
apiVersion: apps/v1
kind: Deployment
metadata:
  name: gitlab-runner
  namespace: gitlab
spec:
  replicas: 1
  selector:
    matchLabels:
      app: gitlab-runner
  template:
    metadata:
      labels:
        app: gitlab-runner
    spec:
      serviceAccountName: gitlab-runner
      containers:
        - name: gitlab-runner
          image: gitlab/gitlab-runner:latest
          imagePullPolicy: IfNotPresent
          env:
            - name: CI_SERVER_URL
              value: "https://git.writeonce.de"  # Replace with your GitLab server URL
            - name: REGISTRATION_TOKEN
              valueFrom:
                secretKeyRef:
                  name: gitlab-runner-secret
                  key: registration-token
          volumeMounts:
            - name: config
              mountPath: /etc/gitlab-runner
      volumes:
        - name: config
          configMap:
            name: gitlab-runner-config
---
apiVersion: v1
kind: ConfigMap
metadata:
  name: gitlab-runner-config
  namespace: gitlab
data:
  config.toml: |
    [[runners]]
      name = "Kubernetes Runner"
      url = "https://git.writeonce.de"  # Replace with your GitLab server URL
      token = ""
      executor = "kubernetes"
      [runners.kubernetes]
        image = "alpine:latest"
        namespace = "gitlab"
        privileged = true
        cpu_request = "100m"
        memory_request = "128Mi"
        cpu_limit = "1"
        memory_limit = "2Gi"
        poll_timeout = 180

        [[runners.kubernetes.volumes.host_path]]
          name = "docker-socket"
          mount_path = "/var/run/docker.sock"
          host_path = "/var/run/docker.sock"
          mount_type = "hostPath"
---
apiVersion: v1
kind: Secret
metadata:
  name: gitlab-runner-secret
  namespace: gitlab
type: Opaque
data:
  registration-token: 


````