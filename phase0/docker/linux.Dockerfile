# SPDX-License-Identifier: AGPL-3.0-only
FROM node:22-bookworm
RUN apt-get update -qq && apt-get install -y -qq cmake && rm -rf /var/lib/apt/lists/*
