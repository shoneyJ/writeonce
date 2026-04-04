# Stage 1: Compile and Build angular codebase

# Use official node image as the base image
FROM node:18.19.0-alpine AS build

# Set the working directory
WORKDIR /app

COPY package*.json ./

# Install all the dependencies
RUN npm install
COPY . .
# Generate the build of the application
RUN npm run build --prod --output=dist

# Stage 2: Create a minimal image with only the dist folder
FROM alpine:latest

# Set the working directory in the new minimal image
WORKDIR /app

# Copy the built application from the previous stage
COPY --from=build /app/dist ./