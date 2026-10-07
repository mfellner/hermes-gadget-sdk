"""GadgetAdapter running on Hermes's real BasePlatformAdapter, with a simulated
device on the other end. The gateway runner is replaced by a small handler, so
these tests pin the adapter contract without needing a model.

Run with a Python that can import Hermes, e.g.:
    HERMES_AGENT_DIR=../hermes-agent ../hermes-agent/.venv/Scripts/python -m pytest tests/test_adapter_hermes.py
"""

from __future__ import annotations

import json
import math
import struct
import types

import pytest

from conftest import requires_hermes, requires_sim

pytestmark = [requires_hermes, requires_sim]

PAIRING_TEXT = ("Hi! I don't recognise you yet. Your pairing code is ABCD2345 (valid for 1 hour). "
                "Ask the owner to run: hermes pairing approve gadget ABCD2345")


class _Ctx:
    """The slice of Hermes's PluginContext that ``register(ctx)`` uses; registers the platform for
    real (PlatformEntry rejects unknown keyword arguments, so this also pins our register() call)."""

    def __init__(self):
        self.tools, self.cli = [], []

    def register_platform(self, name, label, adapter_factory, check_fn, validate_config=None,
                          required_env=None, install_hint="", **kw):
        from gateway.platform_registry import PlatformEntry, platform_registry

        platform_registry.register(PlatformEntry(
            name=name, label=label, adapter_factory=adapter_factory, check_fn=check_fn,
            validate_config=validate_config, required_env=required_env or [], install_hint=install_hint,
            source="builtin", **kw))

    def register_tool(self, **kw):
        self.tools.append(kw["name"])

    def register_cli_command(self, **kw):
        self.cli.append(kw["name"])


@pytest.fixture(scope="module", autouse=True)
def registered_platform():
    import hermes_gadget_plugin

    ctx = _Ctx()
    hermes_gadget_plugin.register(ctx)
    assert set(ctx.tools) == {"gadget_devices", "gadget_display", "gadget_action"}
    assert ctx.cli == ["gadget"]
    return ctx


@pytest.fixture
def gadget(loop_thread, tmp_path, monkeypatch):
    import plugins.plugin_storage as storage
    from gateway.config import PlatformConfig
    from hermes_gadget_plugin.adapter import GadgetAdapter

    monkeypatch.setattr(storage, "plugin_data_dir", lambda name: tmp_path / "plugin-data" / name)
    import gateway.config as gateway_config

    saved_homes = []  # never write the developer's real config.yaml
    monkeypatch.setattr(gateway_config, "persist_home_channel", lambda home, **kw: saved_homes.append(home))
    monkeypatch.delenv("GADGET_HOME_CHANNEL", raising=False)
    adapter = GadgetAdapter(PlatformConfig(enabled=True, extra={"host": "127.0.0.1", "port": 0}))
    state = types.SimpleNamespace(adapter=adapter, authorized=set(), events=[], reply=lambda e: f"echo: {e.text}",
                                  saved_homes=saved_homes)

    adapter.set_authorization_check(
        lambda user_id, chat_type=None, chat_id=None, **kw: user_id in state.authorized)

    async def handler(event):
        # Stands in for the gateway runner: unauthorized senders get a pairing code.
        state.events.append(event)
        if event.source.user_id not in state.authorized:
            await adapter.send(event.source.chat_id, PAIRING_TEXT)
            return None
        return state.reply(event)

    adapter.set_message_handler(handler)
    assert loop_thread.run(adapter.connect())
    state.url = f"ws://127.0.0.1:{adapter.hub.bound_port}/gadget"
    state.run = loop_thread.run
    state.loop = loop_thread.loop
    yield state
    loop_thread.run(adapter.cancel_background_tasks())  # as the gateway does on shutdown
    loop_thread.run(adapter.disconnect())


def _paired_sim(gadget, make_sim, **kw):
    sim = make_sim(gadget.url, **kw)
    assert sim.wait_screen("pairing", "ready", timeout=10)
    gadget.authorized.add(sim.status()["device_id"])
    assert sim.wait_screen("ready", timeout=10)
    return sim


def test_unknown_device_gets_a_pairing_code_then_is_approved(gadget, make_sim):
    sim = make_sim(gadget.url)
    assert sim.wait_for(lambda: sim.status().get("pairing_code") == "ABCD2345", timeout=10)
    assert sim.last_received("pairing")["command"] == "hermes pairing approve gadget ABCD2345"
    trigger = gadget.events[0]
    assert trigger.source.chat_type == "dm" and trigger.source.user_id == sim.status()["device_id"]
    gadget.authorized.add(sim.status()["device_id"])
    assert sim.wait_screen("ready", timeout=10)  # approval is noticed without reconnecting


def test_text_becomes_a_dm_message_event_with_device_context(gadget, make_sim):
    sim = _paired_sim(gadget, make_sim, name="Kitchen")
    gadget.events.clear()
    sim.type_text("what's up")
    assert sim.wait_for(lambda: (sim.last_received("reply") or {}).get("text") == "echo: what's up", timeout=10)
    event = gadget.events[-1]
    assert event.source.platform.value == "gadget"
    assert event.source.chat_id == sim.status()["device_id"]
    assert event.message_type.value == "text"
    assert "Kitchen" in event.channel_prompt and "led.set" in event.channel_prompt
    assert sim.wait_for(lambda: sim.last_received("turn.end") is not None, timeout=5)
    kinds = [m["type"] for m in sim.received]
    assert kinds.index("turn.start") < kinds.index("reply") < kinds.index("turn.end")


def test_voice_arrives_as_a_cached_wav_voice_message(gadget, make_sim):
    import wave

    sim = _paired_sim(gadget, make_sim)
    gadget.events.clear()
    pcm = b"".join(struct.pack("<h", int(6000 * math.sin(i / 5))) for i in range(16000))
    sim.speak_pcm(pcm, tail_s=0.2)
    assert sim.wait_for(lambda: any(e.message_type.value == "voice" for e in gadget.events), timeout=10)
    event = next(e for e in gadget.events if e.message_type.value == "voice")
    assert event.media_types == ["audio/wav"]
    with wave.open(event.media_urls[0]) as w:
        assert w.getframerate() == 16000 and abs(w.getnframes() / 16000 - 1.2) < 0.15


def test_cancel_button_sends_stop(gadget, make_sim):
    sim = _paired_sim(gadget, make_sim)
    gadget.reply = lambda e: None
    gadget.events.clear()
    sim.type_text("long task")
    assert sim.wait_screen("thinking", timeout=5)
    sim.tap("cancel")
    assert sim.wait_for(lambda: any(e.text == "/stop" for e in gadget.events), timeout=5)


def test_new_session_maps_to_slash_new(gadget, make_sim):
    sim = _paired_sim(gadget, make_sim)
    gadget.events.clear()
    sim.console("new-session")
    assert sim.wait_for(lambda: any(e.text == "/new" for e in gadget.events), timeout=5)


def test_transcript_echo_and_interim_messages_are_classified(gadget, make_sim):
    sim = _paired_sim(gadget, make_sim)
    device_id = sim.status()["device_id"]
    gadget.run(gadget.adapter.send(device_id, '\U0001F399️ "turn on the lights"'))
    gadget.run(gadget.adapter.send(device_id, "checking...", metadata={"_interim_send": True}))
    assert sim.wait_for(lambda: sim.last_received("transcript") is not None, timeout=5)
    assert sim.last_received("transcript")["text"] == "turn on the lights"
    assert sim.wait_for(lambda: (sim.last_received("reply") or {}).get("interim") is True, timeout=5)


def test_transcript_echo_is_recognized_in_every_hermes_language(gadget, make_sim, monkeypatch):
    """Hermes translates the echo line (Ukrainian quotes with «»), so the adapter matches the line the
    active language renders rather than one hardcoded form."""
    from agent.i18n import SUPPORTED_LANGUAGES, t

    sim = _paired_sim(gadget, make_sim)
    device_id = sim.status()["device_id"]
    for i, lang in enumerate(SUPPORTED_LANGUAGES):
        monkeypatch.setenv("HERMES_LANGUAGE", lang)
        text = f"lights {i}"
        gadget.run(gadget.adapter.send(device_id, t("gateway.voice.transcript_echo_short", text=text)))
        assert sim.wait_for(lambda: (sim.last_received("transcript") or {}).get("text") == text, timeout=5), lang
    assert sim.last_received("reply") is None


def test_speaking_devices_default_to_spoken_replies(gadget, make_sim):
    talker = _paired_sim(gadget, make_sim, state="talker")
    mute = _paired_sim(gadget, make_sim, state="mute", board="sim-240x240-nospeaker")
    a = gadget.adapter
    assert a._should_auto_tts_for_chat(talker.status()["device_id"]) is True
    assert a._should_auto_tts_for_chat(mute.status()["device_id"]) is False
    a._auto_tts_disabled_chats.add(talker.status()["device_id"])  # what "/voice off" records
    assert a._should_auto_tts_for_chat(talker.status()["device_id"]) is False


def test_whole_file_tts_is_decoded_resampled_and_played(gadget, make_sim, tmp_path):
    from hermes_gadget_plugin import audio

    sim = _paired_sim(gadget, make_sim)
    path = tmp_path / "reply.wav"
    path.write_bytes(audio.wav_bytes(b"\x10\x00" * 24000, 24000))  # 1 s at 24 kHz
    result = gadget.run(gadget.adapter.play_tts(sim.status()["device_id"], str(path)))
    assert result.success
    assert sim.wait_for(lambda: sim.last_received("audio.end") is not None, timeout=10)
    assert sim.last_received("audio.start")["rate"] == 16000
    assert sim.wait_for(lambda: sim.speaker.last_file is not None, timeout=5)
    import wave

    with wave.open(str(sim.speaker.last_file)) as w:
        assert abs(w.getnframes() - 16000) <= 2


def test_streaming_tts_seam_streams_pcm_to_the_device(gadget, make_sim):
    from gateway.platforms.base import AudioFormat

    sim = _paired_sim(gadget, make_sim)
    device_id = sim.status()["device_id"]
    fmt = AudioFormat(sample_rate=24000, channels=1, sample_width=2)
    a = gadget.adapter
    assert a.supports_streaming_tts(device_id, fmt)

    async def stream():
        handle = await a.begin_streaming_tts(device_id, fmt)
        for _ in range(10):
            await a.write_streaming_tts(handle, b"\x20\x00" * 2400)  # 100 ms chunks
        await a.finish_streaming_tts(handle)

    gadget.run(stream())
    assert sim.wait_for(lambda: sim.last_received("audio.end") is not None, timeout=10)
    assert sim.wait_for(lambda: sim.speaker.last_file is not None, timeout=5)
    import wave

    with wave.open(str(sim.speaker.last_file)) as w:
        assert abs(w.getnframes() - 16000) <= 4


def test_agent_tools_drive_the_device(gadget, make_sim, monkeypatch):
    from gateway import session_context
    from hermes_gadget_plugin import tools

    sim = _paired_sim(gadget, make_sim, name="Desk")
    device_id = sim.status()["device_id"]
    monkeypatch.setattr(session_context, "get_session_env",
                        lambda name, default="": {"HERMES_SESSION_PLATFORM": "gadget",
                                                  "HERMES_SESSION_CHAT_ID": device_id}.get(name, default))
    assert tools.available()

    import concurrent.futures

    pool = concurrent.futures.ThreadPoolExecutor(1)  # tools run on agent worker threads

    def call(fn, args):
        fut = pool.submit(fn, args)
        assert sim.wait_for(fut.done, timeout=15)
        return json.loads(fut.result())

    listed = call(tools.gadget_devices, {})
    assert listed["connected"][0]["name"] == "Desk"
    assert "led.set" in [a["name"] for a in listed["connected"][0]["actions"]]

    shown = call(tools.gadget_display, {"title": "**Timer**", "text": "Tea — 3 min", "seconds": 0})
    assert shown["success"]
    assert sim.wait_screen("card", timeout=5)
    assert sim.last_received("display") == {"type": "display", "title": "Timer", "body": "Tea - 3 min", "ttl_s": 0}

    acted = call(tools.gadget_action, {"action": "led.set", "args": {"color": "blue"}})
    assert acted["success"] and acted["result"] == {"led": "blue"} and sim.peripherals.led == "blue"
    missing = call(tools.gadget_action, {"action": "laser.fire"})
    assert not missing["success"] and "no action" in missing["error"]
    pool.shutdown()


def test_images_are_converted_for_the_screen(gadget, make_sim, tmp_path):
    PIL = pytest.importorskip("PIL.Image")
    sim = _paired_sim(gadget, make_sim)
    path = tmp_path / "pic.png"
    PIL.new("RGB", (800, 600), (0, 0, 255)).save(path)
    result = gadget.run(gadget.adapter.send_image_file(sim.status()["device_id"], str(path), caption="A blue square"))
    assert result.success
    start = sim.last_received("image.start") if sim.wait_for(lambda: sim.last_received("image.start"), timeout=5) else None
    assert start and start["width"] <= sim.board.width
    assert sim.wait_screen("image", timeout=5)


def test_offline_devices_report_send_failures(gadget):
    result = gadget.run(gadget.adapter.send("hg-0000000000000000", "hello?"))
    assert not result.success


# -- confirmations and approvals -------------------------------------------------------


def test_confirm_text_keeps_the_header_and_detail():
    from agent.i18n import t
    from hermes_gadget_plugin import textfmt
    from hermes_gadget_plugin.adapter import confirm_text

    message = t("gateway.confirm.destructive_prompt", command="new",
                detail=t("gateway.confirm.detail_new"), prefix="/")
    head, detail = confirm_text("/new", message)
    assert head == "Confirm /new"
    assert detail == textfmt.for_device(t("gateway.confirm.detail_new"))
    assert "/approve" not in detail and "Always" not in detail
    assert confirm_text("/model", "") == ("/model", "")


def _confirming_gateway(gadget, command="new"):
    """Stand in for GatewayRunner._request_slash_confirm: register, then offer the adapter buttons."""
    from tools import slash_confirm

    answers = []

    async def handler(event):
        gadget.events.append(event)
        if event.text != f"/{command}":
            return f"echo: {event.text}"
        session_key = f"agent:main:gadget:dm:{event.source.chat_id}"
        confirm_id = str(len(answers) + len(gadget.events))

        async def on_confirm(choice):
            answers.append(choice)
            return "Cancelled." if choice == "cancel" else "Started a fresh session."

        slash_confirm.register(session_key, confirm_id, command, on_confirm)
        sent = await gadget.adapter.send_slash_confirm(
            chat_id=event.source.chat_id, title=f"/{command}", session_key=session_key, confirm_id=confirm_id,
            message=f"**Confirm /{command}**\n\nThis discards the conversation.\n\nChoose:\n• Approve\n• Cancel")
        return None if sent.success else "text fallback"

    gadget.adapter.set_message_handler(handler)
    return answers


def test_holding_cancel_confirms_its_own_new_session(gadget, make_sim):
    sim = _paired_sim(gadget, make_sim)
    answers = _confirming_gateway(gadget)
    sim.console("new-session")
    assert sim.wait_for(lambda: (sim.last_received("reply") or {}).get("text") == "Started a fresh session.",
                        timeout=5)
    assert answers == ["once"]
    assert sim.last_received("prompt") is None  # nothing to answer on the device


def test_a_typed_new_session_is_confirmed_with_the_buttons(gadget, make_sim):
    sim = _paired_sim(gadget, make_sim)
    answers = _confirming_gateway(gadget)
    sim.type_text("/new")
    assert sim.wait_screen("prompt", timeout=5)
    prompt = sim.last_received("prompt")
    assert prompt["title"] == "Confirm /new" and prompt["text"] == "This discards the conversation."
    sim.run_for(0.7)  # presses right as a question appears are ignored
    sim.tap("cancel")
    assert sim.wait_for(lambda: (sim.last_received("reply") or {}).get("text") == "Cancelled.", timeout=5)
    sim.type_text("/new")
    assert sim.wait_for(lambda: sim.last_received("prompt")["id"] != prompt["id"], timeout=5)
    assert sim.wait_screen("prompt", timeout=5)
    sim.run_for(0.7)
    sim.tap("talk")
    assert sim.wait_for(lambda: (sim.last_received("reply") or {}).get("text") == "Started a fresh session.",
                        timeout=5)
    assert answers == ["cancel", "once"]


def test_command_approvals_queue_on_the_device(gadget, make_sim, monkeypatch):
    import tools.approval as approval

    sim = _paired_sim(gadget, make_sim)
    device_id = sim.status()["device_id"]
    resolved = []
    monkeypatch.setattr(approval, "resolve_gateway_approval",
                        lambda session_key, choice, **kw: resolved.append((session_key, choice)) or 1)

    first = gadget.run(gadget.adapter.send_exec_approval(
        chat_id=device_id, command="rm -rf build", session_key="sk", description="recursive delete"))
    second = gadget.run(gadget.adapter.send_exec_approval(
        chat_id=device_id, command="git push --force", session_key="sk", description="force push"))
    assert first.success and second.success
    assert sim.wait_screen("prompt", timeout=5)
    shown = sim.last_received("prompt")
    assert shown["title"] == "Allow this command?" and "rm -rf build" in shown["text"]

    sim.run_for(0.7)
    sim.tap("talk")
    assert sim.wait_for(lambda: resolved == [("sk", "once")], timeout=5)
    assert sim.wait_for(lambda: "git push --force" in sim.last_received("prompt")["text"], timeout=5)

    # Hermes edits the card when the approval times out: the device drops the question.
    gadget.run(gadget.adapter.edit_message(device_id, second.message_id, "Approval timed out."))
    assert sim.wait_for(lambda: sim.last_received("prompt.close") is not None, timeout=5)
    assert sim.wait_screen("ready", timeout=5)
    assert sim.last_received("notice")["text"] == "Approval timed out."
    assert resolved == [("sk", "once")]


def test_first_approved_device_becomes_the_home_channel(gadget, make_sim):
    first = make_sim(gadget.url, name="Desk", state="first")
    assert first.wait_screen("pairing", timeout=10)
    assert gadget.adapter.config.home_channel is None  # not before approval
    gadget.authorized.add(first.status()["device_id"])
    assert first.wait_screen("ready", timeout=10)
    assert first.wait_for(lambda: gadget.adapter.config.home_channel is not None, timeout=5)
    home = gadget.adapter.config.home_channel
    assert (home.chat_id, home.name, home.platform.value) == (first.status()["device_id"], "Desk", "gadget")
    assert gadget.saved_homes == [home]

    second = _paired_sim(gadget, make_sim, name="Kitchen", state="second")
    second.run_for(0.5)
    assert gadget.adapter.config.home_channel.chat_id == first.status()["device_id"]
    assert len(gadget.saved_homes) == 1


@pytest.mark.parametrize("target", ["explicit-device", "home-channel", "env-home-channel"])
def test_cron_delivers_to_a_paired_gadget(gadget, make_sim, monkeypatch, target):
    from cron import scheduler, scheduler_delivery, scheduler_preflight
    from gateway import config as gateway_config
    from gateway.config import GatewayConfig, Platform

    sim = _paired_sim(gadget, make_sim, name="Desk")
    device_id = sim.status()["device_id"]
    config = GatewayConfig(platforms={Platform("gadget"): gadget.adapter.config})
    monkeypatch.setattr(gateway_config, "load_gateway_config", lambda: config)
    monkeypatch.setattr(scheduler, "load_config", lambda: {"cron": {"wrap_response": False}})
    if target == "env-home-channel":
        gadget.adapter.config.home_channel = None
        monkeypatch.setenv("GADGET_HOME_CHANNEL", device_id)
    elif target == "home-channel":
        assert sim.wait_for(lambda: gadget.adapter.config.home_channel is not None, timeout=5)
    job = {"id": "gadget-reminder", "name": "Reminder",
           "deliver": f"gadget:{device_id}" if target == "explicit-device" else "gadget"}

    assert scheduler_preflight._preflight_check_delivery(job) is None
    targets = scheduler_delivery._resolve_delivery_targets(job)
    assert [(t["platform"], t["chat_id"], t.get("thread_id")) for t in targets] == [
        ("gadget", device_id, None)]
    error = scheduler_delivery._deliver_result(
        job, "Time to stretch", adapters={Platform("gadget"): gadget.adapter}, loop=gadget.loop)
    assert error is None
    assert sim.wait_for(lambda: (sim.last_received("reply") or {}).get("text") == "Time to stretch", timeout=10)


def test_a_home_channel_set_in_the_profile_is_left_alone(gadget, make_sim):
    """GADGET_HOME_CHANNEL may live in a profile's own .env, which a multiplexed gateway keeps out of
    os.environ: the adapter reads it through the profile's secret scope."""
    from agent.secret_scope import reset_secret_scope, set_secret_scope

    sim = make_sim(gadget.url, name="Desk")
    assert sim.wait_screen("pairing", timeout=10)
    session = gadget.adapter.hub.get(sim.status()["device_id"])

    async def claim_in_profile_scope():
        token = set_secret_scope({"GADGET_HOME_CHANNEL": "the-kitchen-device"})
        try:
            await gadget.adapter._claim_home(session)
        finally:
            reset_secret_scope(token)

    gadget.run(claim_in_profile_scope())
    assert gadget.adapter.config.home_channel is None
    assert gadget.saved_homes == []


def _staging(monkeypatch, tmp_path):
    """The queue 'hermes gadget update' writes to, polled quickly."""
    from hermes_gadget_plugin import adapter as adapter_module
    from hermes_gadget_plugin import ota

    monkeypatch.setattr(adapter_module, "UPDATE_POLL_S", 0.2)
    return ota.UpdateQueue(tmp_path / "plugin-data" / "gadget")


def test_staged_firmware_is_installed_once_the_device_is_online(gadget, make_sim, monkeypatch, tmp_path):
    from fakes.fake_firmware import fake_image
    from hermes_gadget_plugin import ota

    queue = _staging(monkeypatch, tmp_path)
    sim = _paired_sim(gadget, make_sim, name="Desk")
    device_id = sim.status()["device_id"]
    image = ota.inspect_image(fake_image(board=sim.board.name, version="0.2.0"))
    queue.stage(device_id, image)
    assert sim.wait_for(lambda: (queue.status(device_id) or {}).get("state") == "done", timeout=30)
    assert queue.status(device_id)["version"] == "0.2.0"
    assert sim.update_image == image.data
    assert queue.pending() == []  # installed once, not again after the restart
    assert sim.wait_for(lambda: sim.restarts == 1, timeout=5)
    assert sim.wait_screen("ready", timeout=15)
    sim.run_for(1.0)
    assert sim.restarts == 1


def test_staged_firmware_the_device_refuses_is_dropped(gadget, make_sim, monkeypatch, tmp_path):
    from fakes.fake_firmware import fake_image
    from hermes_gadget_plugin import ota

    queue = _staging(monkeypatch, tmp_path)
    sim = _paired_sim(gadget, make_sim, name="Desk")
    device_id = sim.status()["device_id"]
    queue.stage(device_id, ota.inspect_image(fake_image(board="esp32s3-breadboard")))
    assert sim.wait_for(lambda: (queue.status(device_id) or {}).get("state") == "failed", timeout=30)
    status = queue.status(device_id)
    assert status["code"] == "wrong_board" and "built for esp32s3-breadboard" in status["error"]
    assert queue.pending() == [] and sim.update_image is None


# -- the device's Hermes menu: status, model, sessions --------------------------------------


class _FakeRunner:
    """The gateway-runner slice the menus read (session store, /status's route resolution)."""

    def __init__(self, title="Weather plans", override=None, running=False):
        self.title = title
        self.override = override if override is not None else {"model": "anthropic/claude-sonnet-5-5",
                                                               "provider": "anthropic"}
        self._running_agents = {"key-dev": object()} if running else {}
        self._session_db = types.SimpleNamespace()
        self.session_store = types.SimpleNamespace(
            lookup_by_session_key=lambda key: types.SimpleNamespace(session_id="sess-1", last_prompt_tokens=1200))

    def _normalize_source_for_session_key(self, source):
        return source

    def _session_key_for_source(self, source):
        return "key-dev"

    async def _status_session_db_facts(self, session_id):
        return self.title, {}, 4567, {}

    def _rehydrate_session_model_override(self, key):
        pass

    def _session_model_override(self, key):
        return self.override

    def _cached_agent_for(self, key):
        return None

    async def _resolve_route_context(self, source, model, route):
        return types.SimpleNamespace(context_length=200000, context_source="models_dev")

    async def _resume_row_visible(self, source, row, allow_all):
        return row.get("id") != "hidden"


@pytest.fixture
def menu_gadget(gadget, monkeypatch):
    """The adapter with a fake runner and a handler that answers the commands menus run."""
    import sys

    # Importing the real gateway.run starts Hermes's entry-point bootstrap (a managed runtime for
    # this test's HERMES_HOME). The menus only need the names /status's route resolution reads.
    stub = types.ModuleType("gateway.run")
    stub._AGENT_PENDING_SENTINEL = object()
    stub._load_gateway_config = lambda *a, **kw: {}
    stub._resolve_gateway_model = lambda config: ""
    monkeypatch.setitem(sys.modules, "gateway.run", stub)
    runner = _FakeRunner()
    adapter = gadget.adapter
    monkeypatch.setattr(adapter, "_runner", lambda: runner)
    gadget.runner, gadget.commands, gadget.picked = runner, [], []

    async def handler(event):
        gadget.events.append(event)
        text = event.text or ""
        if text.startswith(("/model", "/resume", "/new")):
            gadget.commands.append(text)
        if text == "/model":
            async def selected(chat_id, model_id, provider):
                gadget.picked.append((chat_id, model_id, provider))
                return f"Model switched to {model_id}\nProvider: {provider}"

            providers = [
                {"slug": "anthropic", "name": "Anthropic", "is_current": True, "total_models": 2,
                 "models": ["anthropic/claude-sonnet-5-5", "anthropic/claude-opus-5-5"]},
                {"slug": "openai", "name": "OpenAI", "is_current": False, "total_models": 1,
                 "models": ["gpt-6.1"]},
            ]
            await adapter.send_model_picker(event.source.chat_id, providers, "anthropic/claude-sonnet-5-5",
                                            "anthropic", "key-dev", selected)
            return None
        if text.startswith("/resume "):
            return f"Resumed session **{text.split()[1]}**"
        return f"echo: {text}"

    adapter.set_message_handler(handler)
    return gadget


def _menu(sim, title, timeout=10):
    """The labels of the menu the simulator shows (it has UP/DOWN, so no Close row is added)."""
    assert sim.wait_for(lambda: (sim.status().get("menu") or {}).get("title") == title
                        and not sim.status()["menu"]["loading"], timeout=timeout), sim.status().get("menu")
    return sim.status()["menu"]["items"]


def test_paired_devices_get_the_model_and_session_for_the_idle_screen(menu_gadget, make_sim):
    sim = _paired_sim(menu_gadget, make_sim)
    assert sim.wait_for(lambda: sim.status().get("model") == "claude-sonnet-5-5", timeout=10)
    assert sim.status()["session"] == "Weather plans"
    assert sim.last_received("info")["provider"] == "Anthropic"
    # A finished turn may have titled the session: the device hears about it.
    menu_gadget.runner.title = "Trip to Rome"
    sim.type_text("hello")
    assert sim.wait_for(lambda: sim.status().get("session") == "Trip to Rome", timeout=10)


def test_menu_status_is_a_card_without_a_turn(menu_gadget, make_sim):
    sim = _paired_sim(menu_gadget, make_sim)
    assert sim.console("menu") == "@ok menu"
    items = _menu(sim, "Hermes")
    assert items == ["Status", "Model", "Sessions", "New session"]
    assert sim.last_received("menu")["items"][1]["note"] == "claude-sonnet-5-5"
    sim.console("menu pick 0")
    assert sim.wait_screen("card", timeout=10)
    card = sim.last_received("display")
    assert card["title"] == "Status"
    assert "Model: claude-sonnet-5-5" in card["body"] and "Session: Weather plans" in card["body"]
    assert "Context: 1,200 of 200,000 (1%)" in card["body"] and "Tokens used: 4,567" in card["body"]
    assert sim.last_received("turn.start") is None  # no "thinking", nothing spoken
    assert "menu" not in sim.status()


def test_menu_switches_the_model_through_hermes_picker(menu_gadget, make_sim):
    sim = _paired_sim(menu_gadget, make_sim)
    sim.console("menu")
    _menu(sim, "Hermes")
    sim.console("menu pick 1")
    assert _menu(sim, "Provider") == ["Anthropic", "OpenAI", "< Back"]
    assert sim.last_received("menu")["items"][0]["current"] is True
    sim.console("menu pick 0")
    assert _menu(sim, "Anthropic") == ["claude-sonnet-5-5", "claude-opus-5-5", "< Back"]
    assert sim.status()["menu"]["cursor"] == 0  # the current model
    sim.console("menu pick 2")  # back to the providers
    _menu(sim, "Provider")
    sim.console("menu pick 1")
    assert _menu(sim, "OpenAI") == ["gpt-6.1", "< Back"]
    sim.console("menu pick 0")
    assert sim.wait_screen("card", timeout=10)
    assert menu_gadget.picked == [(sim.status()["device_id"], "gpt-6.1", "openai")]
    assert sim.last_received("display")["body"].startswith("Model switched to gpt-6.1")
    assert menu_gadget.commands == ["/model"]
    assert sim.last_received("turn.start") is None


def test_menu_lists_and_resumes_sessions(menu_gadget, make_sim, monkeypatch):
    import time as _time

    import hermes_cli.session_listing as listing

    now = _time.time()
    rows = [
        {"id": "sess-1", "title": "Weather plans", "last_active": now - 120, "is_current_session": True},
        {"id": "sess-0", "title": None, "preview": "what   is a “good” espresso", "last_active": now - 7200},
        {"id": "hidden", "title": "Someone else's", "last_active": now},
        {"id": "20261007_002453_7cdf6fec", "title": None, "preview": "", "started_at": 1791325493.0},
    ]
    calls = []

    def query(db, **kw):
        calls.append(kw)
        return rows

    monkeypatch.setattr(listing, "query_session_listing", query)
    sim = _paired_sim(menu_gadget, make_sim)
    sim.console("menu")
    _menu(sim, "Hermes")
    sim.console("menu pick 2")
    untitled = "Untitled " + _time.strftime("%b %d %H:%M", _time.localtime(1791325493.0))
    assert _menu(sim, "Sessions") == ["New session", "Weather plans", 'what is a "good" espresso', untitled, "< Back"]
    sent = sim.last_received("menu")["items"]
    assert sent[1]["current"] is True and sent[1]["note"] == "2m" and sent[2]["note"] == "2h"
    assert calls[0]["include_unnamed"] is True and calls[0]["session_key"] == "key-dev"
    sim.console("menu pick 1")  # the current session
    assert sim.wait_for(lambda: "menu" not in sim.status(), timeout=10)
    assert sim.last_received("notice")["text"] == "Already in this session"
    sim.console("menu")
    _menu(sim, "Hermes")
    sim.console("menu pick 2")
    _menu(sim, "Sessions")
    sim.console("menu pick 2")
    assert sim.wait_screen("card", timeout=10)
    assert menu_gadget.commands[-1] == "/resume sess-0"
    assert sim.last_received("display") == {**sim.last_received("display"), "title": "Session",
                                            "body": "Resumed session sess-0"}


def test_menu_new_session_and_busy_hermes(menu_gadget, make_sim):
    sim = _paired_sim(menu_gadget, make_sim)
    sim.console("menu")
    _menu(sim, "Hermes")
    sim.console("menu pick 3")
    assert sim.wait_for(lambda: "/new" in menu_gadget.commands, timeout=10)
    assert "menu" not in sim.status()
    # While a turn runs, switching models waits; the list stays for another pick.
    adapter = menu_gadget.adapter
    session = adapter.hub.get(sim.status()["device_id"])
    key = adapter._source_session_key(adapter._source(session))
    adapter._active_sessions[key] = __import__("asyncio").Event()
    try:
        sim.console("menu")
        _menu(sim, "Hermes")
        sim.console("menu pick 1")
        assert sim.wait_for(lambda: (sim.last_received("notice") or {}).get("text", "").startswith("Hermes is busy"),
                            timeout=10)
        assert sim.status()["menu"]["title"] == "Hermes" and not sim.status()["menu"]["loading"]
        assert "/model" not in menu_gadget.commands
    finally:
        adapter._active_sessions.pop(key, None)


def test_without_menus_the_model_picker_falls_back_to_text(menu_gadget):
    session = types.SimpleNamespace(paired=True, has_menus=False)
    menu_gadget.adapter._session = lambda chat_id: session
    result = menu_gadget.run(menu_gadget.adapter.send_model_picker("hg-x", [], "", "", "k", None))
    assert result.success is False
