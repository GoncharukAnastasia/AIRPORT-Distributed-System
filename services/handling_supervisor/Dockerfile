FROM python:3.12-slim

RUN apt-get update && apt-get install -y --no-install-recommends curl && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY services/handling_supervisor/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY services/handling_supervisor/ .

EXPOSE 8085

CMD ["python", "main.py"]
