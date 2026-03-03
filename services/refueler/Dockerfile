FROM python:3.11-slim

RUN apt-get update && apt-get install -y --no-install-recommends curl && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY services/refueler/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY services/refueler/main.py .

EXPOSE 8088

CMD ["python", "main.py"]
