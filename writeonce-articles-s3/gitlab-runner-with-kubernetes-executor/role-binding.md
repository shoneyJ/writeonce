```yaml
apiVersion: rbac.authorization.k8s.io/v1
kind: RoleBinding
metadata:
  name: gitlab-runner-secrets-manager
  namespace: gitlab
subjects:
  - kind: ServiceAccount
    name: gitlab-runner
    namespace: gitlab
roleRef:
  kind: Role
  name: gitlab-runner-secrets-manager
  apiGroup: rbac.authorization.k8s.io


````