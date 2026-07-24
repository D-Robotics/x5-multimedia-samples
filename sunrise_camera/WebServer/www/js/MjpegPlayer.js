/**
 * MjpegPlayer - 通过 ws-fmp4 接收 MJPEG 流，用浏览器原生 JPEG 解码 + canvas 渲染
 *
 * ZL 把 MJPEG 帧封装成 fmp4 segment 通过 ws 发送。每个 fmp4 segment 含 moof+mdat box，
 * mdat 里是一个完整 JPEG 帧。本播放器解析 fmp4 box 提取 mdat 中的 JPEG 数据，
 * 用 createImageBitmap + canvas.drawImage 渲染。
 *
 * 不走 MediaSource（浏览器不支持 MJPEG in MSE），直接用 canvas。
 */

const TIMESTAMP_SCALE = 10;
const DEFAULT_VIDEO_WIDTH = 1920;
const DEFAULT_VIDEO_HEIGHT = 1080;
const POSTER_REVOKE_DELAY_MS = 100;
const FPS_INTERVAL_MS = 1000;

class MjpegPlayer {
	constructor(wsUrl, videoElementId, videoCodecType = 'image/jpeg') {
		this.ws = null;
		this.wsUrl = wsUrl;
		this.videoCodecType = videoCodecType;

		// canvas 元素（由 DisplayWindowManager 创建的 video_render_canvas）
		this.canvas = null;
		this.ctx = null;

		// 备用 video 元素（用于 img 解码 fallback）
		this.videoElement = null;
		this.videoElementId = videoElementId;

		this.isStart = false;
		this.isFirstFrameLoaded = false;
		this.isFirstFrameCallbackDone = false;

		// 帧统计
		this.frameCount = 0;
		this.lastFpsTime = 0;
		this.fps = 0;

		// 时间戳
		this.firstFrameTimeStamp = -1;
		this.lastFrameTimeStamp = -1;

		// JPEG 原始尺寸（poster 模式下 videoWidth/videoHeight 为 0，用这个替代）
		this.bitmapWidth = 0;
		this.bitmapHeight = 0;

		this.userCallbacks = {
			onOpen: null,
			onRecvedFirstFrame: null,
			onFirstFrameLoaded: null,
			onStoped: null,
			onError: null
		};
	}

	init(userCallbacks) {
		this.userCallbacks = userCallbacks;
	}

	start() {
		if (this.ws) {
			const warningMsg = "MjpegPlayer is already running.";
			console.warn(warningMsg);
			return { success: false, message: warningMsg };
		}

		// 用 video 元素（<video> 原生支持 object-fit:contain，自动保持宽高比铺满）
		this.videoElement = document.getElementById(this.videoElementId);
		if (!this.videoElement) {
			const errorMsg = `Element with ID "${this.videoElementId}" not found.`;
			console.error(errorMsg);
			return { success: false, message: errorMsg };
		}

		try {
			this.ws = new WebSocket(this.wsUrl);
			this.ws.binaryType = 'arraybuffer';

			this.ws.addEventListener("message", (e) => this._onWsMessage(e));
			this.ws.addEventListener("open", () => {
				console.log("MjpegPlayer WebSocket connection opened.");
				if (this.userCallbacks.onOpen) {
					this.userCallbacks.onOpen();
				}
			});
			this.ws.addEventListener("close", () => {
				console.log("MjpegPlayer WebSocket connection closed.");
			});
			this.ws.addEventListener("error", (error) => {
				console.error("MjpegPlayer WebSocket error:", error);
				if (this.userCallbacks.onError) {
					this.userCallbacks.onError({ success: false, message: 'WebSocket error' });
				}
			});

			this.isStart = true;
			return { success: true, message: "MjpegPlayer started successfully." };
		} catch (error) {
			console.error("Failed to initialize MjpegPlayer:", error.message);
			return { success: false, message: `Failed: ${error.message}` };
		}
	}

	/**
	 * 按宽高比铺满父容器（参考 WasmVideoPlayer.adjustDisplayWindowSizeDynamically）
	 */
	_adjustCanvasSize(mediaWidth, mediaHeight) {
		var canvas = this.canvas;
		if (!canvas || !canvas.parentElement) return;

		var container = canvas.parentElement;
		var videoRatio = mediaWidth / mediaHeight;
		var containerRatio = container.clientWidth / container.clientHeight;

		var displayWidth, displayHeight;
		if (videoRatio > containerRatio) {
			// 视频更宽 => 宽度撑满
			displayWidth = container.clientWidth;
			displayHeight = displayWidth / videoRatio;
		} else {
			// 视频更高 => 高度撑满
			displayHeight = container.clientHeight;
			displayWidth = displayHeight * videoRatio;
		}

		// 按宽高比设 canvas CSS 尺寸，铺满父容器不变形
		// 用 setProperty+important 覆盖 CSS 的 width:100%!important/height:100%!important
		canvas.style.setProperty('width', displayWidth + 'px', 'important');
		canvas.style.setProperty('height', displayHeight + 'px', 'important');
		// 居中（用像素值，不用 transform，避免和 position:absolute 的 top/left 冲突）
		var leftPx = (container.clientWidth - displayWidth) / 2;
		var topPx = (container.clientHeight - displayHeight) / 2;
		canvas.style.setProperty('left', leftPx + 'px', 'important');
		canvas.style.setProperty('top', topPx + 'px', 'important');
	}

	stop() {
		this.isStart = false;
		try {
			if (this.ws) {
				this.ws.close();
				this.ws = null;
			}
			this.ctx = null;
			this.canvas = null;
			// 撤销 poster 持有的最后一个 blob URL，避免频繁 start/stop 造成 blob URL 泄漏
			if (this.videoElement && this.videoElement.poster && this.videoElement.poster.startsWith('blob:')) {
				URL.revokeObjectURL(this.videoElement.poster);
				this.videoElement.poster = '';
			}
			this.videoElement = null;
			this.isFirstFrameLoaded = false;
			this.isFirstFrameCallbackDone = false;
			console.log("MjpegPlayer stopped successfully.");
			// 通知上层（PlayerWrapper）停止完成，保持与其它播放器一致的停止通知链
			if (this.userCallbacks && typeof this.userCallbacks.onStoped === 'function') {
				this.userCallbacks.onStoped();
			}
			return { success: true, message: "Stopped successfully." };
		} catch (error) {
			console.error("Failed to stop MjpegPlayer:", error);
			return { success: false, message: `Failed: ${error.message}` };
		}
	}

	currentTimeSecond() {
		// 基于最新帧和首帧 timestamp 差值算播放时间（秒）
		// firstFrameTimeStamp/lastFrameTimeStamp 已 ÷10 还原原始 ms
		if (this.firstFrameTimeStamp >= 0 && this.lastFrameTimeStamp >= 0) {
			var diffMs = this.lastFrameTimeStamp - this.firstFrameTimeStamp;
			return diffMs / 1000;
		}
		return 0;
	}

	displayWindowSize() {
		var el = this.canvas || this.videoElement;
		if (!el) return { clientWidth: 0, clientHeight: 0 };
		return { clientWidth: el.clientWidth, clientHeight: el.clientHeight };
	}

	VideoResolutionSize() {
		// poster 模式下 videoElement.videoWidth/videoHeight 为 0，用 bitmap 原始尺寸
		if (this.bitmapWidth > 0 && this.bitmapHeight > 0) {
			return { videoWidth: this.bitmapWidth, videoHeight: this.bitmapHeight };
		}
		if (this.canvas) {
			return { videoWidth: this.canvas.width, videoHeight: this.canvas.height };
		}
		if (this.videoElement && this.videoElement.videoWidth > 0) {
			return { videoWidth: this.videoElement.videoWidth, videoHeight: this.videoElement.videoHeight };
		}
		return { videoWidth: DEFAULT_VIDEO_WIDTH, videoHeight: DEFAULT_VIDEO_HEIGHT };
	}

	///////////////////////////////////////////////////////////////////////////////////
	/**
	 * ws 收到的数据是 fmp4 segment。解析 fmp4 box，提取 mdat 里的 JPEG 数据。
	 * fmp4 segment 结构: moof(含 mfhd/traf/trun) + mdat(含 JPEG 帧数据)
	 */
	_onWsMessage(event) {
		var data = event.data;
		if (!(data instanceof ArrayBuffer)) {
			return;
		}

		var dataView = new DataView(data);
		var offset = 0;
		var jpegData = null;
		var timestamp = 0;

		while (offset < dataView.byteLength - 8) {
			var boxSize = dataView.getUint32(offset);
			var boxType = '';
			for (var i = 0; i < 4; i++) {
				boxType += String.fromCharCode(dataView.getUint8(offset + 4 + i));
			}

			if (boxSize < 8 || offset + boxSize > dataView.byteLength) {
				break;
			}

			if (boxType === 'mdat') {
				// mdat box 包含 JPEG 帧数据
				jpegData = data.slice(offset + 8, offset + boxSize);
			} else if (boxType === 'moof') {
				// 在 moof/traf/trun 里找 tfdt 获取 timestamp
				timestamp = this._parseMoofForTimestamp(dataView, offset, boxSize);
			}

			offset += boxSize;
		}

		if (jpegData) {
			this._renderJpegFrame(jpegData, timestamp);
		}
	}

	/**
	 * 解析 moof box 里的 tfdt，获取 baseMediaDecodeTime 作为时间戳
	 */
	_parseMoofForTimestamp(dataView, moofOffset, moofSize) {
		var offset = moofOffset + 8; // 跳过 moof box header
		var end = moofOffset + moofSize;

		while (offset < end - 8) {
			var boxSize = dataView.getUint32(offset);
			var boxType = '';
			for (var i = 0; i < 4; i++) {
				boxType += String.fromCharCode(dataView.getUint8(offset + 4 + i));
			}

			if (boxSize < 8 || offset + boxSize > end) break;

			if (boxType === 'tfdt') {
				var version = dataView.getUint8(offset + 8);
				var tsValue;
				if (version === 1) {
					// 64-bit，用 getBigUint64 取完整值（和 VideoMSEPlayer 一致）
					tsValue = Number(dataView.getBigUint64(offset + 12));
				} else {
					// 32-bit
					tsValue = dataView.getUint32(offset + 12);
				}
				return tsValue;
			}

			// 递归进入 traf 子 box
			if (boxType === 'traf') {
				var subResult = this._parseMoofForTimestamp(dataView, offset, boxSize);
				if (subResult > 0) return subResult;
			}

			offset += boxSize;
		}

		return 0;
	}

	/**
	 * 渲染 JPEG 帧：用浏览器原生 createImageBitmap 解码 + canvas 绘制
	 */
	async _renderJpegFrame(jpegData, timestamp) {
		try {
			// 乱序帧保护：timestamp 是原始 tfdt(=pts×10)，lastFrameTimeStamp 存的是 ÷10 后的 ms，
			// 比较时把 ms 还原回原始单位，跳过比已渲染帧更早的帧，避免 createImageBitmap 乱序 resolve 造成画面回跳
			if (this.lastFrameTimeStamp >= 0 && timestamp < this.lastFrameTimeStamp * TIMESTAMP_SCALE) {
				return;
			}

			var blob = new Blob([jpegData], { type: 'image/jpeg' });

			// 用 createImageBitmap 解码 JPEG（硬件加速，性能好）
			var bitmap = await createImageBitmap(blob);

			// stop() 可能在 await 期间重置了状态，恢复后必须守卫，避免 stop 后仍渲染/触发首帧回调
			if (!this.isStart) {
				bitmap.close();
				return;
			}

			// 记录 JPEG 原始尺寸（poster 模式下 videoWidth/videoHeight 为 0）
			this.bitmapWidth = bitmap.width;
			this.bitmapHeight = bitmap.height;

			if (!this.isFirstFrameLoaded) {
				this.isFirstFrameLoaded = true;
				this.firstFrameTimeStamp = timestamp / TIMESTAMP_SCALE;
				this.lastFrameTimeStamp = timestamp / TIMESTAMP_SCALE;

				if (this.userCallbacks.onRecvedFirstFrame) {
					// MJPEG 的 tfdt = pts×10，除以 TIMESTAMP_SCALE 还原原始 ms 值，和 bbox timestamp(us) 对齐
					this.userCallbacks.onRecvedFirstFrame(timestamp / TIMESTAMP_SCALE);
				}
			}

			// 每帧更新 lastFrameTimeStamp（÷TIMESTAMP_SCALE 还原原始 ms，用于算法框对齐）
			this.lastFrameTimeStamp = timestamp / TIMESTAMP_SCALE;

			if (this.videoElement) {
				// 用 video.poster 显示 JPEG 帧（<video> 原生 object-fit:contain 自动保持宽高比）
				var url = URL.createObjectURL(blob);
				var oldUrl = this.videoElement.poster;
				this.videoElement.poster = url;
				if (oldUrl && oldUrl.startsWith('blob:')) {
					setTimeout(function () { URL.revokeObjectURL(oldUrl); }, POSTER_REVOKE_DELAY_MS);
				}
			}

			bitmap.close();

			// 帧率统计
			this.frameCount++;
			var now = performance.now();
			if (this.lastFpsTime > 0) {
				var elapsed = now - this.lastFpsTime;
				if (elapsed >= FPS_INTERVAL_MS) {
					this.fps = Math.round(this.frameCount * FPS_INTERVAL_MS / elapsed);
					this.frameCount = 0;
					this.lastFpsTime = now;
				}
			} else {
				this.lastFpsTime = now;
				this.frameCount = 0;
			}

			// 首帧加载完成回调（只回调一次）
			if (!this.isFirstFrameCallbackDone) {
				this.isFirstFrameCallbackDone = true;
				if (this.userCallbacks.onFirstFrameLoaded) {
					this.userCallbacks.onFirstFrameLoaded();
				}
			}

		} catch (error) {
			console.error("MjpegPlayer render error:", error.message);
		}
	}
}

export default MjpegPlayer;
