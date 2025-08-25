```bash
pulumi config set aws:profile writeonce-iac
```

```yaml
name: writeonce-function-s3
description: A minimal AWS Go Pulumi program
runtime: go
config:
  aws:profile: writeonce-iac
  aws:region: eu-central-1

  pulumi:tags:
    value:
      pulumi:template: aws-go
```