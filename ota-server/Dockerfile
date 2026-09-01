FROM python:3.11-alpine

WORKDIR /app

COPY server.py .

ENV OTA_PORT=8081
ENV WEBHOOK_SECRET=
VOLUME /data

EXPOSE 8081

CMD ["python", "-u", "server.py"]
