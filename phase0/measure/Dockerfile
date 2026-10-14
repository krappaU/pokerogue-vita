# SPDX-License-Identifier: AGPL-3.0-only
FROM node:22-bookworm
RUN apt-get update -qq && apt-get install -y -qq chromium fonts-dejavu >/dev/null && rm -rf /var/lib/apt/lists/*
RUN mkdir /opt/pptr && cd /opt/pptr && npm init -y >/dev/null && npm i -q puppeteer-core@23.11.1 >/dev/null 2>&1
