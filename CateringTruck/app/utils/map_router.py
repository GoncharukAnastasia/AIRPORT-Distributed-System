"""
BFS по carRoad-рёбрам карты аэропорта.
Аналог shortest_path_nodes_unsafe() из GroundControl.cpp, но на Python.
"""
import json
import logging
from collections import deque
from typing import Dict, List, Optional

logger = logging.getLogger(__name__)


class AirportMap:
    """Загружает карту аэропорта и строит маршруты через BFS."""

    def __init__(self) -> None:
        # adjacency: node -> list of (neighbor_node, edge_name)
        self._adj: Dict[str, List[tuple]] = {}
        self._loaded = False

    def load(self, path: str) -> None:
        try:
            with open(path, "r", encoding="utf-8") as f:
                doc = json.load(f)
        except Exception as e:
            logger.error("[MapRouter] failed to load map from %s: %s", path, e)
            return

        adj: Dict[str, List[tuple]] = {}

        for edge in doc.get("edges", []):
            name = edge["name"]
            etype = edge.get("type", "")
            n1 = edge["node1"]
            n2 = edge["node2"]

            # Машины катеринга ездят по carRoad и carRoad|planeRoad
            if "carRoad" not in etype:
                continue

            adj.setdefault(n1, []).append((n2, name))
            adj.setdefault(n2, []).append((n1, name))

        self._adj = adj
        self._loaded = True
        logger.info("[MapRouter] loaded %d carRoad edges",
                    sum(len(v) for v in adj.values()) // 2)

    def shortest_path(self, frm: str, to: str) -> Optional[List[str]]:
        """
        Возвращает список узлов от frm до to (включительно) по carRoad-рёбрам.
        Возвращает None если путь не найден или карта не загружена.
        """
        if not self._loaded:
            logger.warning(
                "[MapRouter] map not loaded, cannot route %s -> %s", frm, to)
            return None

        if frm == to:
            return [frm]

        # BFS
        visited = {frm}
        queue: deque = deque()
        queue.append((frm, [frm]))

        while queue:
            node, path = queue.popleft()
            for neighbor, _edge in self._adj.get(node, []):
                if neighbor in visited:
                    continue
                new_path = path + [neighbor]
                if neighbor == to:
                    return new_path
                visited.add(neighbor)
                queue.append((neighbor, new_path))

        logger.warning("[MapRouter] no carRoad path from %s to %s", frm, to)
        return None


# Синглтон — один экземпляр на всё приложение
airport_map = AirportMap()
