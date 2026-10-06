"""On-device menus for the Hermes adapter: status, model switching and sessions.

The device asks for the ``main`` menu and shows whatever lists the server sends
(``menu`` messages); picks come back as ``menu.select``. Everything that changes
Hermes state goes through Hermes's own command handlers (``/model``'s picker
hook, ``/resume <id>``, ``/new``), so the gadget behaves like any other chat.
Reads (status, the session list, the idle ``info`` line) use the gateway
runner's session store and the same route resolution ``/status`` uses; every
one of them fails open, because those are gateway internals.
"""

from __future__ import annotations

import asyncio
import logging
import time
import uuid
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional

from gateway.platforms.base import SendResult
from gateway.platforms.event import MessageType

from . import textfmt
from .hub import DeviceSession

logger = logging.getLogger(__name__)

MAX_SESSIONS = 10      # sessions listed (the device's own, newest first)
MAX_LABEL = 64         # characters per item label (the device fits what it can)
CARD_TTL_S = 60
BUSY_NOTICE = "Hermes is busy - try again after the reply"


@dataclass
class _Menu:
    """The list a device shows, and what its items mean."""
    id: str
    kind: str  # main | providers | models | sessions | choices
    targets: Dict[str, Any] = field(default_factory=dict)  # item id -> provider/model/session/choice
    parent: Optional["_Menu"] = None


@dataclass
class _Picker:
    """Hermes's /model picker callback, while the device browses it."""
    providers: List[dict]
    current_model: str
    current_provider: str
    on_model_selected: Any


def short_model(model: str) -> str:
    """A model id without its vendor path (``anthropic/claude-x`` -> ``claude-x``)."""
    return (model or "").rsplit("/", 1)[-1]


def ago(ts: Any, now: Optional[float] = None) -> str:
    """Compact age of a Unix timestamp: ``now``, ``5m``, ``3h``, ``2d``, ``6w``."""
    try:
        seconds = (now if now is not None else time.time()) - float(ts)
    except (TypeError, ValueError):
        return ""
    if seconds < 60:
        return "now"
    for unit, size in (("w", 604800), ("d", 86400), ("h", 3600), ("m", 60)):
        if seconds >= size:
            return f"{int(seconds // size)}{unit}"
    return ""


def session_label(row: dict, charset: str = "ascii") -> str:
    """How a session appears in the list: its title, else its first words, else when it started."""
    text = row.get("title") or row.get("preview")
    if not text:
        try:
            text = "Untitled " + time.strftime("%b %d %H:%M", time.localtime(float(row.get("started_at"))))
        except (TypeError, ValueError, OverflowError):
            text = row.get("id") or "?"
    return textfmt.for_device(" ".join(str(text).split()), charset)[:MAX_LABEL] or "?"


class GadgetMenus:
    """Mixin for :class:`GadgetAdapter`: the device's Hermes menu and idle info line."""

    _menus: Dict[str, _Menu]
    _pickers: Dict[str, _Picker]

    def _init_menus(self) -> None:
        self._menus = {}
        self._pickers = {}

    # -- plumbing ---------------------------------------------------------------------

    def _runner(self):
        """The gateway runner (None in a bare adapter: reads then fail open)."""
        return getattr(self, "gateway_runner", None)

    async def _run_command(self, session: DeviceSession, text: str) -> Optional[str]:
        """Run a slash command for the device and return Hermes's reply instead of sending it.

        Like the gateway's inline dispatch, without processing hooks: the device shows no
        turn and nothing is spoken.
        """
        if not self._message_handler:
            return None
        event = self._event(session, f"menu-{uuid.uuid4().hex[:6]}", text, message_type=MessageType.TEXT)
        self._canonicalize(event.source)
        text_out, _ttl = self._unwrap_ephemeral(await self._message_handler(event))
        return text_out or None

    def _busy(self, session: DeviceSession) -> bool:
        """A turn is running for this device (commands that switch state must wait)."""
        key = self._source_session_key(self._source(session))
        return key in self._active_sessions

    async def _send_card(self, session: DeviceSession, title: str, text: str) -> None:
        body = textfmt.for_device(text or "", session.charset) or "Done"
        await session.show_card(title, body, ttl_s=CARD_TTL_S)

    async def _show(self, session: DeviceSession, menu: _Menu, title: str, items: List[dict]) -> None:
        self._menus[session.device_id] = menu
        for item in items:
            item["label"] = textfmt.for_device(str(item.get("label") or ""), session.charset)[:MAX_LABEL] or "?"
            if not item.get("note"):
                item.pop("note", None)
            if not item.get("current"):
                item.pop("current", None)
        await session.show_menu(menu.id, title, items)

    @staticmethod
    def _new_menu(kind: str, parent: Optional[_Menu] = None) -> _Menu:
        return _Menu(id=f"{kind[:4]}-{uuid.uuid4().hex[:6]}", kind=kind, parent=parent)

    # -- what Hermes is doing for this device -------------------------------------------

    async def _hermes_state(self, session: DeviceSession) -> Optional[dict]:
        """Model, provider, conversation and context for the device's chat, like /status.

        Read-only: a device that never talked has no session yet and gets none created.
        """
        runner = self._runner()
        if runner is None:
            return None
        try:
            from gateway.run import _AGENT_PENDING_SENTINEL
            from gateway.slash_commands_status import _status_model_route

            source = await asyncio.to_thread(runner._normalize_source_for_session_key, self._source(session))
            key = runner._session_key_for_source(source)
            entry = await asyncio.to_thread(runner.session_store.lookup_by_session_key, key)
            title, row, tokens, route = (None, {}, 0, {})
            if entry is not None:
                title, row, tokens, route = await runner._status_session_db_facts(entry.session_id)
            agent = runner._running_agents.get(key)
            running = agent is not None and agent is not _AGENT_PENDING_SENTINEL
            runner._rehydrate_session_model_override(key)
            override = runner._session_model_override(key) or {}
            model, provider, used, total, _route = _status_model_route(
                agent if running else runner._cached_agent_for(key), override, route or {}, row or {}, entry)
        except Exception as exc:
            logger.debug("[%s] cannot read Hermes state for %s: %s", self.name, session.device_id, exc)
            return None
        label = provider
        try:
            from hermes_cli.providers import get_label

            label = get_label(provider) or provider
        except Exception:
            pass
        return {"model": model or "", "provider": provider or "", "provider_label": label or "",
                "session_id": entry.session_id if entry else "", "title": title or "",
                "context_used": int(used or 0), "context_total": int(total or 0),
                "tokens": int(tokens or 0), "running": running}

    async def _send_info(self, session: DeviceSession) -> None:
        if session.closed or not session.paired or not session.has_menus:
            return
        state = await self._hermes_state(session)
        if state is None:
            return
        await session.send_info(model=short_model(state["model"]), provider=state["provider_label"],
                                session=textfmt.for_device(state["title"], session.charset)[:MAX_LABEL])

    def _refresh_info(self, session: Optional[DeviceSession]) -> None:
        if session is not None and session.paired and session.has_menus:
            session.spawn(self._send_info(session))

    @staticmethod
    def _status_card(state: dict) -> str:
        lines = [f"Model: {short_model(state['model']) or 'unknown'}"]
        if state["provider_label"]:
            lines.append(f"Provider: {state['provider_label']}")
        lines.append(f"Session: {state['title'] or ('untitled' if state['session_id'] else 'none yet')}")
        used, total = state["context_used"], state["context_total"]
        if total:
            lines.append(f"Context: {used:,} of {total:,} ({min(100, round(used * 100 / total))}%)")
        elif used:
            lines.append(f"Context: {used:,} tokens")
        lines.append(f"Tokens used: {state['tokens']:,}")
        if state["running"]:
            lines.append("Working on a reply")
        return "\n".join(lines)

    # -- device requests --------------------------------------------------------------------

    async def on_menu_open(self, session: DeviceSession, menu: str) -> None:
        if not session.paired:
            await session.close_menu("")
            await session.send_notice("Approve pairing first")
            return
        await self._show_main(session)

    async def on_menu_close(self, session: DeviceSession, menu_id: str) -> None:
        current = self._menus.get(session.device_id)
        if current is not None and (not menu_id or current.id == menu_id):
            self._menus.pop(session.device_id, None)
            self._pickers.pop(session.device_id, None)

    async def on_menu_select(self, session: DeviceSession, menu_id: str, item: str) -> None:
        menu = self._menus.get(session.device_id)
        if menu is None or menu.id != menu_id or not session.paired:
            await session.close_menu(menu_id)
            await session.send_notice("That menu has expired")
            return
        try:
            await self._select(session, menu, item)
        except Exception as exc:
            logger.warning("[%s] menu %s/%s failed for %s: %s", self.name, menu.kind, item, session.device_id, exc)
            self._menus.pop(session.device_id, None)
            await session.close_menu(menu_id)
            await self._send_card(session, "Error", f"That did not work: {exc}")

    async def _select(self, session: DeviceSession, menu: _Menu, item: str) -> None:
        if item == "back":
            await self._back(session, menu)
        elif menu.kind == "main":
            await self._select_main(session, menu, item)
        elif menu.kind == "providers":
            await self._show_models(session, menu, menu.targets.get(item))
        elif menu.kind == "models":
            await self._switch_model(session, menu, menu.targets.get(item))
        elif menu.kind == "sessions":
            await self._select_session(session, menu, item)
        elif menu.kind == "choices":
            await self._choose(session, menu, menu.targets.get(item))

    async def _back(self, session: DeviceSession, menu: _Menu) -> None:
        parent = menu.parent
        picker = self._pickers.get(session.device_id)
        if parent is not None and parent.kind == "providers" and picker and len(picker.providers) > 1:
            await self._show_providers(session, parent.parent)
            return
        self._pickers.pop(session.device_id, None)  # leaving the model picker
        await self._show_main(session)

    def _finish(self, session: DeviceSession, menu: _Menu):
        """Leave the menu: the device closes it (a card or notice follows)."""
        self._menus.pop(session.device_id, None)
        self._pickers.pop(session.device_id, None)
        return session.close_menu(menu.id)

    # -- main -------------------------------------------------------------------------------

    async def _show_main(self, session: DeviceSession) -> None:
        state = await self._hermes_state(session) or {}
        model = short_model(state.get("model", ""))
        title = state.get("title", "")
        items = [
            {"id": "status", "label": "Status", "note": "busy" if state.get("running") else ""},
            {"id": "model", "label": "Model", "note": model},
            {"id": "sessions", "label": "Sessions", "note": textfmt.for_device(title, session.charset)[:24]},
            {"id": "new", "label": "New session"},
        ]
        await self._show(session, self._new_menu("main"), "Hermes", items)

    async def _select_main(self, session: DeviceSession, menu: _Menu, item: str) -> None:
        if item == "status":
            state = await self._hermes_state(session)
            text = self._status_card(state) if state else await self._run_command(session, "/status")
            await self._finish(session, menu)
            await self._send_card(session, "Status", text or "Hermes did not report a status")
        elif item == "model":
            if self._busy(session):
                await session.send_notice(BUSY_NOTICE)
                return
            # /model with no arguments calls send_model_picker below when Hermes can list models.
            self._pickers.pop(session.device_id, None)
            reply = await self._run_command(session, "/model")
            if session.device_id not in self._pickers:
                await self._finish(session, menu)
                await self._send_card(session, "Model", reply or "Hermes listed no models")
        elif item == "sessions":
            await self._show_sessions(session, menu)
        elif item == "new":
            await self._finish(session, menu)
            await self.on_new_session(session)  # /new, confirmed (the device asked for it)

    # -- model ------------------------------------------------------------------------------

    async def send_model_picker(self, chat_id: str, providers: list, current_model: str, current_provider: str,
                                session_key: str, on_model_selected,
                                metadata: Optional[Dict[str, Any]] = None) -> SendResult:
        """Hermes's /model picker hook: browse providers, then models, on the device."""
        session = self._session(chat_id)
        if session is None or not session.paired or not session.has_menus:
            return SendResult(success=False, error="this gadget has no menus")
        usable = [p for p in providers or [] if isinstance(p, dict) and p.get("models")]
        if not usable:
            return SendResult(success=False, error="no models to pick from")
        self._pickers[session.device_id] = _Picker(usable, current_model or "", current_provider or "",
                                                   on_model_selected)
        main = self._menus.get(session.device_id)
        root = main if main is not None and main.kind == "main" else None
        await self._show_providers(session, root)
        return SendResult(success=True, message_id=uuid.uuid4().hex[:12])

    async def _show_providers(self, session: DeviceSession, parent: Optional[_Menu]) -> None:
        picker = self._pickers.get(session.device_id)
        if picker is None:
            await self._show_main(session)
            return
        menu = self._new_menu("providers", parent)
        if len(picker.providers) == 1:  # nothing to choose: straight to the models
            await self._show_models(session, menu, picker.providers[0])
            return
        items = []
        for i, p in enumerate(picker.providers):
            menu.targets[f"p{i}"] = p
            items.append({"id": f"p{i}", "label": p.get("name") or p.get("slug") or "?",
                          "note": str(p.get("total_models") or len(p.get("models") or [])),
                          "current": bool(p.get("is_current"))})
        items.append({"id": "back", "label": "< Back"})
        await self._show(session, menu, "Provider", items)

    async def _show_models(self, session: DeviceSession, parent: _Menu, provider: Optional[dict]) -> None:
        picker = self._pickers.get(session.device_id)
        if picker is None or provider is None:
            await self._show_main(session)
            return
        menu = self._new_menu("models", parent)
        current = picker.current_model if provider.get("is_current") else ""
        items = []
        for i, model in enumerate(provider.get("models") or []):
            model_id = str(model.get("id") if isinstance(model, dict) else model)
            menu.targets[f"m{i}"] = (model_id, str(provider.get("slug") or ""))
            items.append({"id": f"m{i}", "label": short_model(model_id),
                          "current": bool(current) and model_id == current})
        items.append({"id": "back", "label": "< Back"})
        await self._show(session, menu, textfmt.for_device(str(provider.get("name") or "Models"), session.charset), items)

    async def _switch_model(self, session: DeviceSession, menu: _Menu, target: Optional[tuple]) -> None:
        picker = self._pickers.get(session.device_id)
        if picker is None or target is None:
            await self._show_main(session)
            return
        if self._busy(session):
            await session.send_notice(BUSY_NOTICE)
            return
        model_id, provider_slug = target
        await self._finish(session, menu)
        result = await picker.on_model_selected(session.device_id, model_id, provider_slug)
        await self._send_card(session, "Model", result or f"Switched to {short_model(model_id)}")
        self._refresh_info(session)

    # -- other pickers (/reasoning, /fast, ...) ---------------------------------------------

    async def send_choice_picker(self, chat_id: str, title: str, choices: list, session_key: str,
                                 on_choice_selected, metadata: Optional[Dict[str, Any]] = None) -> SendResult:
        session = self._session(chat_id)
        if session is None or not session.paired or not session.has_menus or not choices:
            return SendResult(success=False, error="this gadget has no menus")
        menu = self._new_menu("choices")
        menu.targets["_callback"] = on_choice_selected
        items = []
        for i, choice in enumerate(choices):
            menu.targets[f"c{i}"] = str(choice.get("value") or "")
            items.append({"id": f"c{i}", "label": str(choice.get("label") or choice.get("value") or "?"),
                          "current": bool(choice.get("is_current"))})
        heading = textfmt.for_device(title or "Choose", session.charset).split("\n", 1)[0][:32] or "Choose"
        await self._show(session, menu, heading, items)
        return SendResult(success=True, message_id=uuid.uuid4().hex[:12])

    async def _choose(self, session: DeviceSession, menu: _Menu, value: Optional[str]) -> None:
        callback = menu.targets.get("_callback")
        if value is None or callback is None:
            return
        await self._finish(session, menu)
        await self._send_card(session, "Hermes", await callback(session.device_id, value))
        self._refresh_info(session)

    # -- sessions ---------------------------------------------------------------------------

    async def _list_sessions(self, session: DeviceSession) -> List[dict]:
        """This device's most recent sessions (titled or not), newest first, as /sessions full scopes them."""
        runner = self._runner()
        db = getattr(runner, "_session_db", None)
        if db is None:
            return []
        from hermes_cli.session_listing import query_session_listing

        source = await asyncio.to_thread(runner._normalize_source_for_session_key, self._source(session))
        key = runner._session_key_for_source(source)
        entry = await asyncio.to_thread(runner.session_store.lookup_by_session_key, key)
        rows = await asyncio.to_thread(
            query_session_listing, getattr(db, "_db", db),
            source=source.platform.value if source.platform else None, session_key=key,
            current_session_id=entry.session_id if entry else None, include_current_session=True,
            include_unnamed=True, limit=MAX_SESSIONS, exclude_sources=["tool"])
        return [row for row in rows if await runner._resume_row_visible(source, row, allow_all=False)]

    async def _show_sessions(self, session: DeviceSession, parent: _Menu) -> None:
        try:
            rows = await self._list_sessions(session)
        except Exception as exc:
            logger.debug("[%s] cannot list sessions for %s: %s", self.name, session.device_id, exc)
            rows = []
        menu = self._new_menu("sessions", parent)
        items = [{"id": "new", "label": "New session"}]
        now = time.time()
        for i, row in enumerate(rows[:MAX_SESSIONS]):
            menu.targets[f"s{i}"] = row
            items.append({"id": f"s{i}", "label": session_label(row, session.charset),
                          "note": ago(row.get("last_active") or row.get("started_at"), now),
                          "current": bool(row.get("is_current_session"))})
        items.append({"id": "back", "label": "< Back"})
        await self._show(session, menu, "Sessions", items)

    async def _select_session(self, session: DeviceSession, menu: _Menu, item: str) -> None:
        if item == "new":
            await self._finish(session, menu)
            await self.on_new_session(session)
            return
        row = menu.targets.get(item)
        if row is None:
            return
        if row.get("is_current_session"):
            await self._finish(session, menu)
            await session.send_notice("Already in this session")
            return
        if self._busy(session):
            await session.send_notice(BUSY_NOTICE)
            return
        await self._finish(session, menu)
        reply = await self._run_command(session, f"/resume {row['id']}")
        await self._send_card(session, "Session", reply or f"Resumed {session_label(row, session.charset)}")
        self._refresh_info(session)
