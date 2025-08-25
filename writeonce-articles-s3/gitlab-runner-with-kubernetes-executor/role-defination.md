```yaml
apiVersion: rbac.authorization.k8s.io/v1
kind: Role
metadata:
  name: gitlab-runner-secrets-manager
  namespace: gitlab
rules:
- apiGroups: [""]
  resources: ["events"]
  verbs:
  - "list"
  - "watch" # Required when FF_PRINT_POD_EVENTS=true
- apiGroups: [""]
  resources: ["namespaces"]
  verbs:
  - "create" # Required when kubernetes.NamespacePerJob=true
  - "delete" # Required when kubernetes.NamespacePerJob=true
- apiGroups: [""]
  resources: ["pods"]
  verbs:
  - "create"
  - "delete"
  - "get"
  - "list" # Required when FF_USE_INFORMERS=true
  - "watch" # Required when FF_KUBERNETES_HONOR_ENTRYPOINT=true, FF_USE_INFORMERS=true, FF_USE_LEGACY_KUBERNETES_EXECUTION_STRATEGY=false
- apiGroups: [""]
  resources: ["pods/attach"]
  verbs:
  - "create" # Required when FF_USE_LEGACY_KUBERNETES_EXECUTION_STRATEGY=false
  - "delete" # Required when FF_USE_LEGACY_KUBERNETES_EXECUTION_STRATEGY=false
  - "get" # Required when FF_USE_LEGACY_KUBERNETES_EXECUTION_STRATEGY=false
  - "patch" # Required when FF_USE_LEGACY_KUBERNETES_EXECUTION_STRATEGY=false
- apiGroups: [""]
  resources: ["pods/exec"]
  verbs:
  - "create"
  - "delete"
  - "get"
  - "patch"
- apiGroups: [""]
  resources: ["pods/log"]
  verbs:
  - "get" # Required when FF_KUBERNETES_HONOR_ENTRYPOINT=true, FF_USE_LEGACY_KUBERNETES_EXECUTION_STRATEGY=false, FF_WAIT_FOR_POD_TO_BE_REACHABLE=true
  - "list" # Required when FF_KUBERNETES_HONOR_ENTRYPOINT=true, FF_USE_LEGACY_KUBERNETES_EXECUTION_STRATEGY=false
- apiGroups: [""]
  resources: ["secrets"]
  verbs:
  - "create"
  - "delete"
  - "get"
  - "update"
- apiGroups: [""]
  resources: ["serviceaccounts"]
  verbs:
  - "get"
- apiGroups: [""]
  resources: ["services"]
  verbs:
  - "create"
  - "get"

````