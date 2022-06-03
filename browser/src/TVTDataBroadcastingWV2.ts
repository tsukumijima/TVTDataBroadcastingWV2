import { ResponseMessage } from "../web-bml/server/ws_api";
import { BMLBrowser, BMLBrowserFontFace, EPG, Indicator, IP, InputApplication, InputCancelReason, InputCharacterType } from "../web-bml/client/bml_browser";
import { decodeTS } from "../web-bml/server/decode_ts";
import { CaptionPlayer } from "../web-bml/client/player/caption_player";

export class StatusBarIndicator implements Indicator {
    private readonly receivingStatusElement: HTMLElement;
    private readonly networkingStatusElement: HTMLElement;
    constructor(receivingStatusElement: HTMLElement, networkingStatusElement: HTMLElement) {
        this.receivingStatusElement = receivingStatusElement;
        this.networkingStatusElement = networkingStatusElement;
    }
    url = "";
    receiving = false;
    eventName: string | null = "";
    loading = false;
    networkingPost = false;
    private update() {
        (window as any).chrome.webview.postMessage({
            type: "status",
            loading: this.loading,
            url: this.url,
            receiving: this.receiving,
        } as FromWebViewMessage);
        if (this.receiving) {
            this.receivingStatusElement.style.display = "";
        } else {
            this.receivingStatusElement.style.display = "none";
        }
        if (this.networkingPost) {
            this.networkingStatusElement.style.display = "";
        } else {
            this.networkingStatusElement.style.display = "none";
        }
    }
    public setUrl(name: string, loading: boolean): void {
        this.url = name;
        this.loading = loading;
        this.update();
    }
    public setReceivingStatus(receiving: boolean): void {
        this.receiving = receiving;
        this.update();
    }
    public setNetworkingGetStatus(get: boolean): void {

    }
    public setNetworkingPostStatus(post: boolean): void {
        this.networkingPost = post;
        this.update();
    }
    public setEventName(eventName: string | null): void {
        this.eventName = eventName;
        this.update();
    }
}

// BML文書と動画と字幕が入る要素
const browserElement = document.getElementById("data-broadcasting-browser")!;
// 動画が入っている要素
const videoContainer = browserElement.querySelector(".arib-video-container") as HTMLElement;
// BML文書が入る要素
const contentElement = browserElement.querySelector(".data-broadcasting-browser-content") as HTMLElement;
// BML用フォント
const roundGothic: BMLBrowserFontFace = { source: "url('../dist/KosugiMaru-Regular.ttf'), url('/rounded-mplus-1m-arib.ttf'), local('MS Gothic')" };
const boldRoundGothic: BMLBrowserFontFace = { source: "url('../dist/KosugiMaru-Regular.ttf'), url('/rounded-mplus-1m-arib.ttf'), local('MS Gothic')" };
const squareGothic: BMLBrowserFontFace = { source: "url('../dist/Kosugi-Regular.ttf'), url('/rounded-mplus-1m-arib.ttf'), local('MS Gothic')" };
const ccContainer = browserElement.querySelector(".arib-video-cc-container") as HTMLElement;
const player = new CaptionPlayer(document.createElement("video"), ccContainer);
// リモコン
const remoteControl = new StatusBarIndicator(browserElement.querySelector(".remote-control-receiving-status")!, browserElement.querySelector(".remote-control-networking-status")!);

const ccStatus = document.getElementById("cc-status")!;

const ccStatusAnimation = ccStatus.animate([{ visibility: "hidden", offset: 1 }], {
    duration: 2000,
    fill: "forwards",
});

const epg: EPG = {
    tune(originalNetworkId, transportStreamId, serviceId) {
        console.error("tune", originalNetworkId, transportStreamId, serviceId);
        (window as any).chrome.webview.postMessage({
            type: "tune",
            originalNetworkId,
            transportStreamId,
            serviceId,
        } as FromWebViewMessage);
        return true;
    }
};

let networkEnabled = false;

const apiIP: IP = {
    getConnectionType() {
        return 403;
    },
    isIPConnected() {
        return networkEnabled ? 1 : 0;
    },
    async transmitTextDataOverIP(uri, body) {
        if (!networkEnabled) {
            return { resultCode: NaN, statusCode: "", response: new Uint8Array() };
        }
        try {
            const res = await window.fetch("https://tvtdatabroadcastingwv2-api.invalid/api/post/" + uri, {
                method: "POST",
                headers: {
                    "Content-Type": "application/x-www-form-urlencoded"
                },
                body,
            });
            return { resultCode: 1, statusCode: res.status.toString(), response: new Uint8Array(await res.arrayBuffer()) };
        } catch {
            return { resultCode: NaN, statusCode: "", response: new Uint8Array() };
        }
    },
    async get(uri) {
        if (!networkEnabled) {
            return {};
        }
        try {
            const res = await window.fetch("https://tvtdatabroadcastingwv2-api.invalid/api/get/" + uri, {
                method: "GET",
            });
            return { statusCode: res.status, headers: res.headers, response: new Uint8Array(await res.arrayBuffer()) };
        } catch {
            return {};
        }
    },
};

const audioContext = new AudioContext();
const gainNode = audioContext.createGain();
gainNode.connect(audioContext.destination);


let changeCallback: ((value: string) => void) | undefined;

const inputApplication: InputApplication = {
    launch(opts) {
        if (changeCallback != null) {
            this.cancel("other");
        }
        (window as any).chrome.webview.postMessage({
            type: "input",
            characterType: opts.characterType,
            maxLength: opts.maxLength,
            value: opts.value,
            allowedCharacters: opts.allowedCharacters,
            inputMode: opts.inputMode,
        } as FromWebViewMessage);
        changeCallback = opts.callback;
    },
    cancel(reason) {
        if (changeCallback != null) {
            changeCallback = undefined;
            (window as any).chrome.webview.postMessage({
                type: "cancelInput",
                reason,
            } as FromWebViewMessage);
        }
    },
};

const bmlBrowser = new BMLBrowser({
    containerElement: contentElement,
    mediaElement: videoContainer,
    indicator: remoteControl,
    fonts: {
        roundGothic,
        boldRoundGothic,
        squareGothic
    },
    epg,
    videoPlaneModeEnabled: true,
    audioNodeProvider: {
        getAudioDestinationNode() {
            return gainNode;
        }
    },
    ip: apiIP,
    inputApplication,
    greg: {
        getReg(index) {
            return window.sessionStorage.getItem(`Greg[${index}]`) ?? "";
        },
        setReg(index, value) {
            window.sessionStorage.setItem(`Greg[${index}]`, value);
        },
    },
});

// trueであればデータ放送の上に動画を表示させる非表示状態
bmlBrowser.addEventListener("invisible", (evt) => {
    console.log("invisible", evt.detail);
    if (evt.detail) {
        contentElement.style.clipPath = "inset(0px)";
    } else {
        contentElement.style.clipPath = "";
    }
    (window as any).chrome.webview.postMessage({
        type: "invisible",
        invisible: evt.detail,
    } as FromWebViewMessage);
});

bmlBrowser.addEventListener("load", (evt) => {
    console.log("load", evt.detail);
    const width = evt.detail.resolution.width + "px";
    const aspectNum = evt.detail.displayAspectRatio.numerator;
    const aspectDen = evt.detail.displayAspectRatio.denominator;
    const scaleY = (evt.detail.resolution.width / evt.detail.resolution.height) / (aspectNum / aspectDen);
    const transform = `scaleY(${scaleY})`;
    const height = (evt.detail.resolution.height * scaleY) + "px";
    if (browserElement.style.width !== width || browserElement.style.height !== height || contentElement.style.transform !== transform) {
        browserElement.style.width = width;
        browserElement.style.height = height;
        contentElement.style.transform = transform;
        ccContainer.style.width = width;
        ccContainer.style.height = height;
        onResized();
    }
});

type FromWebViewMessage = {
    type: "videoChanged",
    left: number,
    top: number,
    right: number,
    bottom: number,
    invisible: boolean,
} | {
    type: "status",
    url: string,
    receiving: boolean,
    loading: boolean,
} | {
    type: "invisible",
    invisible: boolean,
} | {
    type: "tune",
    originalNetworkId: number,
    transportStreamId: number,
    serviceId: number,
} | {
    type: "nvramRead",
    filename: string,
    structure: string,
    data: any[] | null,
} | {
    type: "usedKeyList",
    usedKeyList: { [key: string]: boolean },
} | {
    type: "cancelInput",
    reason: InputCancelReason,
} | {
    type: "input",
    characterType: InputCharacterType,
    allowedCharacters?: string,
    maxLength: number,
    value: string,
    inputMode: "text" | "password",
};

bmlBrowser.addEventListener("videochanged", (evt) => {
    const { left, top, right, bottom } = evt.detail.boundingRect;
    (window as any).chrome.webview.postMessage({
        type: "videoChanged",
        left: left * window.devicePixelRatio,
        top: top * window.devicePixelRatio,
        right: right * window.devicePixelRatio,
        bottom: bottom * window.devicePixelRatio,
        invisible: bmlBrowser.content.invisible ?? true,
    } as FromWebViewMessage);
});

bmlBrowser.addEventListener("usedkeylistchanged", (evt) => {
    const { usedKeyList } = evt.detail; (window as any).chrome.webview.postMessage({
        type: "usedKeyList",
        usedKeyList: Object.fromEntries([...usedKeyList.values()].map(x => [x, true])),
    } as FromWebViewMessage);
});

let pcr: number | undefined;

function onMessage(msg: ResponseMessage) {
    if (msg.type === "pes") {
        player.push(msg.streamId, Uint8Array.from(msg.data), msg.pts);
    } else if (msg.type === "pcr") {
        pcr = (msg.pcrBase + msg.pcrExtension / 300) / 90;
    }
    bmlBrowser.emitMessage(msg);
}

const tsStream = decodeTS({
    sendCallback: onMessage,
    serviceId: Number(new URL(location.href).searchParams.get("serviceId")) || undefined,
    parsePES: true
});

tsStream.on("data", () => {});

type ToWebViewMessage = {
    type: "stream",
    data: number[],
    time?: number,
} | {
    type: "key",
    keyCode: number,
} | {
    type: "caption",
    enable: boolean,
    showIndicator: boolean,
} | {
    type: "nvramRead",
    filename: string,
    structure: string,
} | {
    type: "nvramWrite",
    filename: string,
    structure: string,
    data: any[],
} | {
    type: "volume",
    value: number,
} | {
    type: "nvramDelete",
} | {
    type: "enableNetwork",
    enable: boolean,
} | {
    type: "changeInput",
    value: string,
} | {
    type: "cancelInput",
};

function onWebViewMessage(data: ToWebViewMessage, reply: (data: FromWebViewMessage) => void) {
    if (data.type === "stream") {
        const ts = data.data;
        const prevPCR = pcr;
        tsStream.parse(Buffer.from(ts));
        const curPCR = pcr;
        if (prevPCR !== curPCR && curPCR != null) {
            player.updateTime(curPCR - 450);
        }
    } else if (data.type === "key") {
        bmlBrowser.content.processKeyDown(data.keyCode);
        bmlBrowser.content.processKeyUp(data.keyCode);
    } else if (data.type === "caption") {
        if (data.enable) {
            if (data.showIndicator) {
                ccStatus.style.display = "";
                ccStatus.textContent = "字幕オン";
                ccStatusAnimation.cancel();
                ccStatusAnimation.play();
            }
            player.showCC();
        } else {
            if (data.showIndicator) {
                ccStatus.style.display = "";
                ccStatus.textContent = "字幕オフ";
                ccStatusAnimation.cancel();
                ccStatusAnimation.play();
            }
            player.hideCC();
        }
    } else if (data.type === "nvramRead") {
        reply({
            type: "nvramRead",
            filename: data.filename,
            structure: data.structure,
            data: bmlBrowser.nvram.readPersistentArray(data.filename, data.structure),
        });
    } else if (data.type === "nvramWrite") {
        bmlBrowser.nvram.writePersistentArray(data.filename, data.structure, data.data, undefined, true);
    } else if (data.type === "volume") {
        gainNode.gain.value = data.value;
    } else if (data.type === "nvramDelete") {
        const keys: string[] = [];
        for (let i = 0; i < window.localStorage.length; i++) {
            const key = window.localStorage.key(i);
            if (key != null && key.startsWith("nvram_")) {
                keys.push(key);
            }
        }
        for (const key of keys) {
            window.localStorage.removeItem(key);
        }
    } else if (data.type === "enableNetwork") {
        networkEnabled = data.enable;
    } else if (data.type === "changeInput") {
        changeCallback?.(data.value);
        changeCallback = undefined;
    } else if (data.type === "cancelInput") {
        changeCallback = undefined;
    }
}

(window as any).chrome.webview.addEventListener("message", (ev: any) => onWebViewMessage(ev.data as ToWebViewMessage, (window as any).chrome.webview.postMessage));

(window as any).sendMessage = (data: ToWebViewMessage): FromWebViewMessage | null => {
    let result: FromWebViewMessage | null = null;
    onWebViewMessage(data, (data) => result = data);
    return result;
};

function onResized() {
    const windowWidth = document.documentElement.clientWidth;
    const windowHeight = document.documentElement.clientHeight;
    const contentWidth = browserElement.clientWidth;
    const contentHeight = browserElement.clientHeight;
    const scaleX = windowWidth / contentWidth;
    const scaleY = windowHeight / contentHeight;
    const s = Math.min(scaleX, scaleY);
    document.body.style.transform = `translate(${(contentWidth * s + windowWidth) / 2 - contentWidth * s}px, ${(contentHeight * s + windowHeight) / 2 - contentHeight * s}px) scale(${s})`;
    document.body.style.transformOrigin = `0px 0px`;
    const { left, top, right, bottom } = videoContainer.getBoundingClientRect();
    (window as any).chrome.webview.postMessage({
        type: "videoChanged",
        left: left * window.devicePixelRatio,
        top: top * window.devicePixelRatio,
        right: right * window.devicePixelRatio,
        bottom: bottom * window.devicePixelRatio,
        invisible: bmlBrowser.content.invisible ?? true,
    } as FromWebViewMessage);
}

window.addEventListener("resize", onResized);

onResized();
