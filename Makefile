HOST   ?= 0.0.0.0
PORT   ?= 8000

.DEFAULT_GOAL := help

.PHONY: help install run test lint format check clean

help: ## Show available targets
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) \
		| awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-12s\033[0m %s\n", $$1, $$2}'

install: ## Install all dependencies (runtime + dev)
	uv sync --all-groups

run: ## Start the server with live reload  (HOST/PORT overridable)
	uv run uvicorn main:app --host $(HOST) --port $(PORT) --reload

test: ## Run the test suite
	uv run pytest tests/ -v

lint: ## Check code with ruff
	uv run ruff check .

format: ## Auto-format code with ruff
	uv run ruff format .

check: lint test ## Run lint then tests (CI gate)

clean: ## Remove generated files (.venv, caches, snapshots)
	rm -rf .venv uv.lock .pytest_cache .ruff_cache
	find . -type d -name __pycache__ -exec rm -rf {} +
	find . -name "*.pyc" -delete
