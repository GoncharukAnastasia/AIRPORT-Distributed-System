FROM python:3.12-slim

RUN apt-get update && apt-get install -y --no-install-recommends curl && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY services/airport_visualizer/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY services/airport_visualizer/ .
COPY data/ /app/data/

EXPOSE 8087

CMD ["python", "main.py"]
