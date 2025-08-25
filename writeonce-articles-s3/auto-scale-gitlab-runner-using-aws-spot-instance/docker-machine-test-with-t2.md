```bash
docker-machine create \
  --driver=amazonec2 \
  --amazonec2-access-key=$AWS_ACCESS_KEY_ID \
  --amazonec2-secret-key=$AWS_SECRET_ACCESS_KEY \
  --amazonec2-region=$AWS_DEFAULT_REGION \
  --amazonec2-vpc-id=$AWS_VPC_ID \
  --amazonec2-subnet-id=$AWS_SUBNET_ID \
  --amazonec2-zone=$AWS_ZONE \
  --amazonec2-use-private-address=false \  
  --amazonec2-security-group=$AWS_SECURITY_GROUP \ 
  --amazonec2-request-spot-instance=true \
  --amazonec2-spot-price=0.005 \
  --amazonec2-instance-type=t2.micro \
  --amazonec2-ami=ami-02584c1c9d05efa69 \
   --amazonec2-ssh-user=$AWS_SSH_USER \
  --amazonec2-keypair-name=$AWS_KEYPAIR_NAME \
  --amazonec2-ssh-keypath=$AWS_SSH_KEYPATH \
  test-gitlab-docker-machine

````