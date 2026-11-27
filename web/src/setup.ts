// OFT Node flasher (index.html): install the firmware on a board and set its
// pins. Everything after that (channels, thresholds, laps) is on the Timer's
// own page. The Widscan58 dashboard is scanner.html (main.ts).
// GPL-3.0-or-later.
import "./styles.css";
import { el } from "./util/canvas";
import { registerServiceWorker } from "./util/pwa";
import { Store, parseInfo } from "./model/store";
import { App } from "./model/app";
import { Device, WebSerialTransport, grantedEspressifPort, requestEspressifPort } from "./serial/device";
import { FlashDialog } from "./views/flashDialog";
import { GpioDialog } from "./views/gpioDialog";

const store = new Store();
const app = new App(store);

// ---- Theme: the system's unless one was chosen (shared with the scanner page) ----
const THEME_KEY = "widscan58.theme";
const themeMeta = document.querySelector('meta[name="theme-color"]');
function showTheme(t: string) {
  document.documentElement.dataset.theme = t;
  themeMeta?.setAttribute("content", t === "light" ? "#f6f7f9" : "#111417");
}
let themeName = matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark";
try {
  themeName = localStorage.getItem(THEME_KEY) ?? themeName;
} catch {
  /* storage unavailable */
}
showTheme(themeName);
document.body.classList.add("setup");
registerServiceWorker();

// ---- Page -----------------------------------------------------------------------
const dot = el("span", { class: "dot" });
const connText = el("span", {}, "No board");
const connectBtn = el("button", {}, "Connect");
const boardVersion = el("dd", {}, "not connected");
const newVersion = el("dd", {}, "…");
const flashBtn = el("button", { class: "primary" }, "Flash firmware…");
const gpioBtn = el("button", { disabled: "" }, "Set pins…") as HTMLButtonElement;
const gpioHint = el("span", { class: "muted" }, "Connect a flashed board first.");
const themeBtn = el("button", { class: "link" }, "");
const noSerial = el(
  "div",
  { class: "banner", style: "display:none" },
  "This browser cannot reach USB serial devices. Use Chrome or Edge on a computer, or Chrome on Android with a USB OTG adapter.",
);
const problem = el("div", { class: "banner", style: "display:none" });

const step = (n: number, title: string, ...body: (Node | string)[]) =>
  el("section", {}, el("h2", {}, el("span", { class: "step-n" }, String(n)), title), ...body);

document.getElementById("app")!.append(
  el(
    "header",
    { class: "top" },
    el("span", { class: "brand" }, el("h1", {}, "OFT Node")),
    el("span", { class: "conn-state pill" }, dot, connText),
    el("span", { class: "actions" }, connectBtn),
  ),
  el(
    "main",
    {},
    el(
      "p",
      { class: "lead" },
      "Turns a Seeed Studio XIAO ESP32-C5 into a Node of an ",
      el("a", { href: "https://github.com/Open-FPV-Timer/oft", target: "_blank", rel: "noopener" }, "Open FPV Timer"),
      ": the part that listens to the video channels. Flash it here once; after that the Timer runs it.",
    ),
    noSerial,
    problem,
    step(
      1,
      "Flash the firmware",
      el("p", {}, "Plug the board into this computer with a USB data cable."),
      el("dl", { class: "kv" }, el("dt", {}, "On the board"), boardVersion, el("dt", {}, "This page installs"), newVersion),
      el("div", { class: "row" }, flashBtn),
    ),
    step(
      2,
      "Set the pins, if yours differ",
      el(
        "p",
        {},
        "The defaults match the OFT wiring: serial to the Timer on D6 (TX) and D7 (RX), and the SYNC input on D8 " +
          "(GPIO8). Move them here if your build uses other pins.",
      ),
      el("div", { class: "row" }, gpioBtn, gpioHint),
    ),
    step(
      3,
      "Wire it to the Timer",
      el(
        "p",
        {},
        "Connect the Node to the Timer and open the Timer's own page. Channels, thresholds, laps and later firmware " +
          "updates are all done there: you do not need this page again.",
      ),
    ),
  ),
  el(
    "footer",
    {},
    el("a", { href: "https://github.com/Open-FPV-Timer/oft-node-c5", target: "_blank", rel: "noopener" }, "Source"),
    el("a", { href: "scanner.html", title: "Spectrum, lap timer and power meter views over USB" }, "Scanner tools"),
    el("a", { href: "https://github.com/CapnBry/widscan58", target: "_blank", rel: "noopener" }, "Based on Widscan58"),
    themeBtn,
  ),
);

const syncThemeBtn = () => (themeBtn.textContent = themeName === "light" ? "Dark theme" : "Light theme");
syncThemeBtn();
themeBtn.onclick = () => {
  themeName = themeName === "light" ? "dark" : "light";
  showTheme(themeName);
  syncThemeBtn();
  try {
    localStorage.setItem(THEME_KEY, themeName);
  } catch {
    /* storage unavailable */
  }
};

function showProblem(text: string) {
  problem.textContent = text;
  problem.style.display = text ? "block" : "none";
}

function render() {
  const connected = !!app.device;
  const info = connected ? parseInfo(store.info) : null;
  dot.className = `dot ${connected ? "ok" : ""}`;
  connText.textContent = connected ? "Board connected" : "No board";
  connectBtn.textContent = connected ? "Disconnect" : "Connect";
  boardVersion.textContent = !connected ? "not connected" : (info?.version ?? "unknown");
  gpioBtn.disabled = !connected;
  gpioHint.hidden = connected;
}
store.subscribe(render);

// ---- Connection -------------------------------------------------------------------
let userDisconnected = false;
let flashing = false; // keeps auto-reconnect off the port meanwhile

async function connect(transport: WebSerialTransport) {
  const device = new Device(transport);
  store.info = "";
  device.onItem((item) => store.ingest(item));
  device.onClose = () => {
    if (app.device === device) app.device = null;
    render();
  };
  try {
    await device.open();
  } catch (e) {
    showProblem(`Could not open the board: ${(e as Error).message}.`);
    return;
  }
  app.device = device;
  showProblem("");
  render();
  // The firmware starts silent and answers commands; a board a Timer is
  // talking to (OFT mode) answers nothing on USB until it restarts.
  try {
    await device.command("INFO", 1500);
  } catch {
    if (app.device === device)
      showProblem(
        "The board is connected but does not answer. If it has no OFT Node firmware yet, flash it. " +
          "If a Timer is running it, press RESET on the board, or disconnect it from the Timer, and connect again.",
      );
  }
  render();
}

async function disconnect() {
  const d = app.device;
  if (!d) return;
  app.device = null;
  await d.close().catch(() => {});
  render();
}

async function autoConnect() {
  if (app.device || userDisconnected || flashing) return;
  const port = await grantedEspressifPort();
  if (port && !app.device) await connect(new WebSerialTransport(port));
}

connectBtn.onclick = () => {
  if (app.device) {
    userDisconnected = true;
    return void disconnect();
  }
  userDisconnected = false;
  void connect(new WebSerialTransport());
};

// ---- Flashing and pins ----------------------------------------------------------------
const flashDialog = new FlashDialog({
  async takePort() {
    const t = app.device?.transport;
    const port = t instanceof WebSerialTransport && t.currentPort ? t.currentPort : null;
    await disconnect();
    const chosen = port ?? (await requestEspressifPort()); // throws if the chooser is dismissed
    flashing = true;
    return chosen;
  },
  device() {
    return app.device ? parseInfo(store.info) : null;
  },
  finished(ok) {
    flashing = false;
    if (!ok) return;
    // The board re-enumerates after its reset; autoConnect also runs on that event.
    userDisconnected = false;
    setTimeout(() => void autoConnect(), 1500);
  },
});
const gpioDialog = new GpioDialog(app);
document.body.append(flashDialog.el, gpioDialog.el);
store.subscribe(() => flashDialog.refreshDevice());
flashBtn.onclick = () => flashDialog.open();
gpioBtn.onclick = () => gpioDialog.open();
void flashDialog.offeredVersion().then((v) => (newVersion.textContent = v ?? "unavailable"));

if (WebSerialTransport.supported()) {
  WebSerialTransport.onConnectEvent(() => setTimeout(() => void autoConnect(), 500));
  void autoConnect();
} else {
  noSerial.style.display = "block";
  connectBtn.disabled = flashBtn.disabled = true;
}
render();
