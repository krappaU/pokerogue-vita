# SPDX-License-Identifier: AGPL-3.0-only
# The log server (devserver.py, standard library only). Built by compose.yaml.
FROM python:3.13.7-slim
COPY devserver.py /app/devserver.py
ENV DEVSERVER_DIR=/data PYTHONUNBUFFERED=1
EXPOSE 8765
CMD ["python3", "/app/devserver.py"]
