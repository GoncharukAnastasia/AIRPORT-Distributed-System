FROM python:3.11-slim

RUN apt-get update && apt-get install -y --no-install-recommends curl && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY services/catering_truck/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY services/catering_truck/catering_truck.py .

EXPOSE 8089

CMD ["python", "catering_truck.py"]
