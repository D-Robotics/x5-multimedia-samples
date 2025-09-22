import PlayerWrapper from './PlayerWrapper.js';
import ImageQualityParam from './ImageQualityParam.js'
class ImageQualityControler {
	static DOM_CLASSES = {
		VIDEO_CONTAINER: 'video-player-container',
		VIDEO_ELEMENT: 'video-player',
		CANVAS_ELEMENT: 'canvas-video-player',
		VIDEO_OVERLAY: 'video-overlay',
	};

	constructor() {
		// 核心属性：DOM元素引用
		this.dialog = null;
		// 模式状态管理
		this.paramStateDevice = new ImageQualityParam();
		// GUI正在显示的模式
		this.modeStateGUI = {
			image: 'auto',		// 图像调节模式
			exposure: 'auto',	 // 曝光调节模式
			whiteBalance: 'auto', // 白平衡调节模式
			nr2d: 'auto',		 // 空域降噪模式
			nr3d: 'auto'		// 时域降噪模式
		};
		// GUI 正在显示的参数值
		this.paramStateGUI = {
			// 图像调节参数
			image: {
				brightness: { value: 50, min: 0, max: 100, unit: '%' },
				contrast: { value: 50, min: 0, max: 100, unit: '%' },
				saturation: { value: 50, min: 0, max: 100, unit: '%' },
				sharpness: { value: 50, min: 0, max: 100, unit: '%' }
			},
			// 曝光调节参数
			exposure: {
				exposureTime: { value: 50, min: 0, max: 100, unit: '%' },
				again: { value: 50, min: 0, max: 100, unit: '%' },
				dgain: { value: 50, min: 0, max: 100, unit: '%' }
			},
			// 白平衡调节参数
			whiteBalance: {
				redGain: { value: 50, min: 0, max: 100, unit: '%' },
				blueGain: { value: 50, min: 0, max: 100, unit: '%' }
			},
			// 图像增强参数
			enhance: {
				nr2d: {
					level: { value: 3, min: 0, max: 100, unit: '' }
				},
				nr3d: {
					level: { value: 3, min: 0, max: 100, unit: '' }
				}
			}
		};
		// 播放器相关属性
		this.player = null;
		this.pipelineChannel = -1;
		this.playerIsStarted = false;
		this.videoElement = null;
		this.canvasVideoElement = null;
		this.pipelineChannel = null;
		this.browserCapabilities = null;
		this.userCallbacks = {
			onReturn: null,
			onSetISPParam: null,
			onSyncISPAutoModeParam: null,
		};
		this.autoModeSyncTimer = null;
	}

	init(mediaServerIPAddr, browserCapabilities, callbacks = {}){
		this.browserCapabilities = browserCapabilities;
		this.userCallbacks = callbacks;
		this.mediaServerIPAddr = mediaServerIPAddr;
	}

	/**
	 * 将 ImageQualityParam 中的参数同步到 paramStateGUI
	 * 根据当前模式（自动/手动）选择对应的参数源
	 */
	syncParamStateToDefault() {
		// 处理图像参数
		const imageMode = this.paramStateDevice.image.mode;
		const imageSource = imageMode === 'manual'
			? this.paramStateDevice.image.manual
			: this.paramStateDevice.image.state;

		this.paramStateGUI.image = {
			brightness: {
				...this.paramStateGUI.image.brightness,
				value: imageSource.brightness
			},
			contrast: {
				...this.paramStateGUI.image.contrast,
				value: imageSource.contrast
			},
			saturation: {
				...this.paramStateGUI.image.saturation,
				value: imageSource.saturation
			},
			sharpness: {
				...this.paramStateGUI.image.sharpness,
				value: imageSource.sharpness
			}
		};

		// 处理曝光参数
		const exposureMode = this.paramStateDevice.exposure.mode;
		const exposureSource = exposureMode === 'manual'
			? this.paramStateDevice.exposure.manual
			: this.paramStateDevice.exposure.state;

		this.paramStateGUI.exposure = {
			exposureTime: {
				...this.paramStateGUI.exposure.exposureTime,
				value: exposureSource.exposureTime
			},
			again: {
				...this.paramStateGUI.exposure.again,
				value: exposureSource.again
			},
			dgain: {
				...this.paramStateGUI.exposure.dgain,
				value: exposureSource.dgain
			}
		};

		// 处理白平衡参数
		const awbMode = this.paramStateDevice.whiteBalance.mode;
		const awbSource = awbMode === 'manual'
			? this.paramStateDevice.whiteBalance.manual
			: this.paramStateDevice.whiteBalance.state;

		this.paramStateGUI.whiteBalance = {
			redGain: {
				...this.paramStateGUI.whiteBalance.redGain,
				value: awbSource.redGain
			},
			blueGain: {
				...this.paramStateGUI.whiteBalance.blueGain,
				value: awbSource.blueGain
			}
		};

		// 处理空域降噪参数
		const nr2dMode = this.paramStateDevice.nr2d.mode;
		const nr2dSource = nr2dMode === 'manual'
			? this.paramStateDevice.nr2d.manual
			: this.paramStateDevice.nr2d.state;

		this.paramStateGUI.enhance.nr2d.level = {
			...this.paramStateGUI.enhance.nr2d.level,
			value: nr2dSource.level
		};

		// 处理时域降噪参数
		const nr3dMode = this.paramStateDevice.nr3d.mode;
		const nr3dSource = nr3dMode === 'manual'
			? this.paramStateDevice.nr3d.manual
			: this.paramStateDevice.nr3d.state;

		this.paramStateGUI.enhance.nr3d.level = {
			...this.paramStateGUI.enhance.nr3d.level,
			value: nr3dSource.level
		};

		// 同步模式状态
		this.modeStateGUI = {
			image: imageMode,
			exposure: exposureMode,
			whiteBalance: awbMode,
			nr2d: nr2dMode,
			nr3d: nr3dMode
		};
	}
	/**
	 * 将 paramStateGUI 中的值刷新到UI控件（修复组内所有数值显示）
	 */
	refreshUIControls() {
		// 定义参数组与UI的映射关系
		const paramGroups = [
			{
				groupKey: 'image',
				params: ['brightness', 'contrast', 'saturation', 'sharpness'],
				dataAttr: 'group'
			},
			{
				groupKey: 'exposure',
				params: ['exposureTime', 'again', 'dgain'],
				dataAttr: 'group'
			},
			{
				groupKey: 'whiteBalance',
				params: ['redGain', 'blueGain'],
				dataAttr: 'group'
			},
			{
				groupKey: 'nr2d',
				params: ['level'],
				dataAttr: 'enhance',
				parentKey: 'enhance'
			},
			{
				groupKey: 'nr3d',
				params: ['level'],
				dataAttr: 'enhance',
				parentKey: 'enhance'
			}
		];

		// 刷新每个参数组的UI
		paramGroups.forEach(({ groupKey, params, dataAttr, parentKey }) => {
			// 更新模式选择下拉框
			const modeSelect = document.getElementById(`${groupKey}-mode-select`);
			if (modeSelect && modeSelect.value !== this.modeStateGUI[groupKey]) {
				modeSelect.value = this.modeStateGUI[groupKey];
			}
			// 获取当前组的所有参数项容器（每个参数对应一个容器）
			const paramItems = document.querySelectorAll(`.${groupKey}-param`);
			// 为每个参数项找到对应的容器并更新
			params.forEach((paramKey, index) => {
				// 获取当前参数对应的容器（按索引匹配）
				const paramContainer = paramItems[index];
				if (!paramContainer) {
					console.warn(`未找到参数容器: ${groupKey}-${paramKey}`);
					return;
				}
				// 获取参数信息
				const paramConfig = parentKey
					? this.paramStateGUI[parentKey][groupKey][paramKey]
					: this.paramStateGUI[groupKey][paramKey];
				// 查找当前参数项内的滑动条
				const slider = paramContainer.querySelector(`[data-${dataAttr}="${groupKey}"][data-param="${paramKey}"]`);
				// 查找当前参数项内的数值显示（容器内唯一的.param-value）
				const valueDisplay = paramContainer.querySelector('.param-value');
				// 更新滑动条
				if (slider) {
					slider.value = paramConfig.value;
					slider.disabled = this.modeStateGUI[groupKey] !== 'manual';
					slider.classList.toggle('enabled', this.modeStateGUI[groupKey] === 'manual');
					slider.classList.toggle('disabled', this.modeStateGUI[groupKey] !== 'manual');
				}
					// 更新数值显示
				if (valueDisplay) {
					valueDisplay.textContent = `${paramConfig.value}${paramConfig.unit}`;
				}
			});
		});
	}
	/**
	 * 刷新指定组的所有UI控件（滑动条+数值显示+模式下拉框）
	 * @param {string} groupKey - 组标识（如 image/nr2d）
	 * @param {string} [parentKey=''] - 父级参数键（增强组为 'enhance'，普通组为空）
	 */
	refreshGroupUIControls(groupKey, parentKey = '') {
		// 1. 定位当前组的所有参数配置
		let groupParams = {};
		let paramKeys = [];
		if (parentKey === 'enhance') {
			// 增强组：参数在 paramStateGUI.enhance[groupKey] 下（仅level）
			groupParams = this.paramStateGUI.enhance[groupKey];
			paramKeys = Object.keys(groupParams); // 固定为 ['level']
		} else {
			// 普通组：参数在 paramStateGUI[groupKey] 下（如 brightness/contrast）
			groupParams = this.paramStateGUI[groupKey];
			paramKeys = Object.keys(groupParams);
		}

		// 2. 获取当前组的所有参数容器（每个参数对应一个容器）
		const paramContainers = document.querySelectorAll(`.${groupKey}-param`);
		if (paramContainers.length !== paramKeys.length) {
			console.warn(`参数容器数量不匹配: 预期 ${paramKeys.length} 个，实际 ${paramContainers.length} 个`);
		}

		// 3. 遍历所有参数，按索引匹配容器并刷新对应UI
		paramKeys.forEach((paramKey, index) => {
			// 获取当前参数对应的容器（按索引匹配）
			const paramContainer = paramContainers[index];
			if (!paramContainer) {
				console.warn(`未找到参数容器: ${groupKey}-${paramKey}（索引: ${index}）`);
				return;
			}

			// 获取当前参数的完整配置（值、单位、min/max）
			const paramConfig = parentKey === 'enhance'
				? groupParams[paramKey]
				: groupParams[paramKey];

			// 4. 刷新滑动条（区分普通组和增强组的data属性）
			const dataAttr = parentKey || 'group'; // 普通组用data-group，增强组用data-enhance
			const slider = paramContainer.querySelector(`[data-${dataAttr}="${groupKey}"][data-param="${paramKey}"]`);
			if (slider) {
				slider.value = paramConfig.value; // 同步参数值
				slider.disabled = this.modeStateGUI[groupKey] !== 'manual'; // 同步禁用状态
				slider.classList.toggle('enabled', this.modeStateGUI[groupKey] === 'manual');
				slider.classList.toggle('disabled', this.modeStateGUI[groupKey] !== 'manual');
			} else {
				console.warn(`未找到滑动条: ${groupKey}-${paramKey}（索引: ${index}）`);
			}

			// 5. 刷新数值显示（容器内唯一的.param-value）
			const valueDisplay = paramContainer.querySelector('.param-value');
			if (valueDisplay) {
				valueDisplay.textContent = `${paramConfig.value}${paramConfig.unit}`;
			} else {
				console.warn(`未找到数值显示: ${groupKey}-${paramKey}（索引: ${index}）`);
			}
		});

		// 6. 刷新模式下拉框（确保选中状态与当前模式一致）
		const modeSelect = document.getElementById(`${groupKey}-mode-select`);
		if (modeSelect && modeSelect.value !== this.modeStateGUI[groupKey]) {
			modeSelect.value = this.modeStateGUI[groupKey];
		}
	}

	/**
	 * 对外接口：创建并显示图像质量控制对话框
	 * @param {number} videoNum - 目标视频窗口编号
	 */
	handleImageQualityControl(videoNum, codec_type) {
		if (this.dialog) {
			this.destroyDialog();
		}

		this.pipelineChannel = videoNum;
		this.createDialog();
		this.loadVideoStream(codec_type);
		this.startAutoModeSyncTimer();
	}
	handleISPSetParamResult(videoNum, ispParams){
		if (videoNum != this.pipelineChannel) {
			console.log(`videoNum is error, current is ${this.pipelineChannel}, but recv ${videoNum}`);
			return;
		}

		const { groupKey, configs } = ispParams;
		this.paramStateDevice.fromGroupJSON(groupKey, configs, false);
	}
	handleISPGetAllParam(videoNum, ispParams) {
		if (videoNum != this.pipelineChannel) {
			console.log(`videoNum is error, current is ${this.pipelineChannel}, but recv ${videoNum}`);
			return;
		}

		// 1. 先更新 paramState 实例
		this.paramStateDevice.fromJSON(ispParams);

		// 2. 步骤a：将 ImageQualityParam 中的参数同步到 paramStateGUI
		this.syncParamStateToDefault();

		// 3. 步骤b：将 paramStateGUI 中的值刷新到UI控件
		this.refreshUIControls();
	}

	handleISPUpdateAutoParam(videoNum, ispParams) {
		if (videoNum != this.pipelineChannel) {
			console.log(`videoNum is error, current is ${this.pipelineChannel}, but recv ${videoNum}`);
			return;
		}

		// 1. 找到所有处于自动模式的组
		const autoGroups = Object.entries(this.modeStateGUI)
			.filter(([groupKey, mode]) => mode === 'auto')
			.map(([groupKey]) => groupKey);

		if (autoGroups.length === 0) {
			return; // 没有自动模式的组，无需更新
		}

		// 2. 针对每个自动模式组，从ispParams中获取state值并更新到paramStateGUI
		autoGroups.forEach(groupKey => {
			// 检查ispParams中是否存在该组的状态数据
			if (!ispParams[groupKey] || !ispParams[groupKey].state) {
				console.warn(`ISP参数中缺少组${groupKey}的state数据`);
				return;
			}
			const groupState = ispParams[groupKey].state;
			const isEnhanceGroup = ['nr2d', 'nr3d'].includes(groupKey);
			// 根据是否为增强组（nr2d/nr3d）使用不同的参数路径
			if (isEnhanceGroup) {
				// 处理增强组参数（只有level一个参数）
				if (groupState.level !== undefined) {
					this.paramStateGUI.enhance[groupKey].level.value = groupState.level;
				}
			} else {
				// 处理普通组参数（image/exposure/whiteBalance）
				Object.entries(this.paramStateGUI[groupKey]).forEach(([paramKey, paramConfig]) => {
					if (groupState[paramKey] !== undefined) {
						paramConfig.value = groupState[paramKey];
					}
				});
			}
			console.log(`update auto group [${groupKey}]`);
			// 3. 刷新该组的UI控件
			this.refreshGroupUIControls(groupKey, isEnhanceGroup ? 'enhance' : '');
		});
	}
	syncAutoModeParamToCallback() {
		if (typeof this.userCallbacks.onSyncISPAutoModeParam !== 'function') {
			console.warn('未配置 onSyncISPAutoModeParam 回调，跳过自动同步');
			return;
		}
		// 构建需要同步的自动模式参数（包含设备参数和GUI状态）
		const autoModeParam = {
			video_id: Number(this.pipelineChannel),
			timestamp: Date.now() // 时间戳（用于标识数据时效性）
		};
		// 调用用户回调，传递自动模式参数
		this.userCallbacks.onSyncISPAutoModeParam(autoModeParam);
	}

	startAutoModeSyncTimer() {
		// 先清除旧定时器（避免重复创建）
		this.stopAutoModeSyncTimer();
		 console.log('自动模式参数同步定时器已启动');

		// 创建新定时器：每1秒执行一次同步逻辑
		this.autoModeSyncTimer = setInterval(() => {
			this.syncAutoModeParamToCallback();
		}, 1000); // 1000ms = 1秒
	}
	stopAutoModeSyncTimer() {
		if (this.autoModeSyncTimer) {
			clearInterval(this.autoModeSyncTimer); // 清除定时器
			this.autoModeSyncTimer = null; // 重置定时器实例
			console.log('自动模式参数同步定时器已停止');
		}
	}


	/* ===================================	页面操作	 ================================== */
	/**
	 * 创建完整的对话框结构
	 */
	createDialog() {
		// 1. 创建遮罩层
		this.dialog = document.createElement('div');
		this.dialog.className = 'image-quality-dialog';
		this.dialog.id = 'image-quality-dialog';
		// 添加基础字体样式
		this.dialog.style.fontSize = '14px';

		// 2. 创建对话框内容区
		const dialogContent = document.createElement('div');
		dialogContent.className = 'dialog-content';

		// 3. 组装对话框结构：标题栏 → 主体内容 → 底部按钮
		dialogContent.appendChild(this.createHeader());
		dialogContent.appendChild(this.createBody());
		dialogContent.appendChild(this.createFooter());

		// 4. 添加到页面
		this.dialog.appendChild(dialogContent);
		document.body.appendChild(this.dialog);
	}

	/**
	 * 创建对话框标题栏
	 * @returns {HTMLElement} 标题栏元素
	 */
	createHeader() {
		const header = document.createElement('div');
		header.className = 'dialog-header';
		header.style.fontSize = '16px'; // 标题稍大一些

		// 标题文本
		const titleText = document.createElement('span');
		titleText.textContent = `图像质量控制 - Camera 接口(CSI${this.pipelineChannel-1})`;

		// 模式提示
		const modeTip = document.createElement('span');
		modeTip.className = 'mode-tip';
		modeTip.textContent = '自动模式下参数不可调节';
		modeTip.style.fontSize = '12px'; // 提示文字稍小

		header.appendChild(titleText);
		header.appendChild(modeTip);
		return header;
	}

	/**
	 * 创建对话框主体内容（视频预览 + 参数配置）
	 * @returns {HTMLElement} 主体内容元素
	 */
	createBody() {
		const body = document.createElement('div');
		body.className = 'dialog-body';

		// 左侧视频播放器
		body.appendChild(this.createVideoPlayer());

		// 右侧配置面板
		body.appendChild(this.createConfigPanel());

		return body;
	}

	/**
	 * 创建视频播放器（无任何控件，仅用于预览）
	 * @returns {HTMLElement} 视频容器元素
	 */
	createVideoPlayer() {
		const container = document.createElement('div');
		container.className = ImageQualityControler.DOM_CLASSES.VIDEO_CONTAINER;

		// 视频元素: 播放H264
		this.videoElement = document.createElement('video');
		this.videoElement.className = ImageQualityControler.DOM_CLASSES.VIDEO_ELEMENT;
		this.videoElement.id = ImageQualityControler.DOM_CLASSES.VIDEO_ELEMENT + '_0';
		this.videoElement.controls = false;
		this.videoElement.muted = true;
		this.videoElement.autoplay = true;
		this.videoElement.playsInline = true;
		this.videoElement.oncontextmenu = (e) => e.preventDefault();

		// 创建 视频显示canvas 元素： 播放H265
		this.canvasVideoElement = document.createElement("canvas");
		this.canvasVideoElement.className = ImageQualityControler.DOM_CLASSES.CANVAS_ELEMENT;
		this.canvasVideoElement.id = ImageQualityControler.DOM_CLASSES.CANVAS_ELEMENT + '_0';

		// 透明覆盖层
		const overlay = document.createElement('div');
		overlay.className = ImageQualityControler.DOM_CLASSES.VIDEO_OVERLAY;

		container.appendChild(this.videoElement);
		container.appendChild(this.canvasVideoElement);
		container.appendChild(overlay);
		return container;
	}
	getPlayerDisplayElementId(pipelineChannel, displayElementType){
		if(displayElementType === "video"){
			return ImageQualityControler.DOM_CLASSES.VIDEO_ELEMENT + '_0';
		}else if(displayElementType === "canvas"){
			return ImageQualityControler.DOM_CLASSES.CANVAS_ELEMENT + '_0';
		}else{
			return `unsupport_${pipelineChannel}`
		}
	}
	createAndStartPlayer(codec_type){
		console.log(`createAndStartPlayer: ${this.pipelineChannel}.`);
		//Player
		const userCallbacks = {
			onOpen: (window_index) => {},
			onRecvedFirstFrame: (window_index, timestamp) => {},
			onFirstFrameLoaded:(window_index) =>{},
			onStoped: (window_index) => {},
			onError: (window_index) => {},
		}
		this.player = new PlayerWrapper(this.browserCapabilities, this.pipelineChannel, userCallbacks);
		const stream_type = ( codec_type === 'h265') ? 'sub1' : 'main';
		let wsUrl = `ws://${this.mediaServerIPAddr}:8080/ch${this.pipelineChannel - 1}/${stream_type}.live.mp4`;
		this.player.init(wsUrl, codec_type,
					this.getPlayerDisplayElementId.bind(this));
		this.player.start();
		this.playerIsStarted = true;
	}
	destroyAndStopPlayer(){
		if(this.player && this.playerIsStarted){
			this.player.stop();
			this.playerIsStarted = false;
			this.player = null;
		}else{
			console.log(`${this.pipelineChannel}player is not stared, so ignore stopPlayer.`);
		}
	}
	/**
	 * 加载目标视频流
	 */
	loadVideoStream(codec_type) {
		this.createAndStartPlayer(codec_type);
	}

	/**
	 * 显示视频占位图
	 */
	showVideoPlaceholder() {
		if (!this.videoElement) return;

		// 清空视频容器
		const container = this.videoElement.parentElement;
		container.innerHTML = '';

		// 创建并添加占位图
		const placeholder = document.createElement('img');
		placeholder.className = 'video-placeholder';
		placeholder.src = 'data:image/svg+xml;base64,PHN2ZyB3aWR0aD0iMjAwIiBoZWlnaHQ9IjEyMCIgeG1sbnM9Imh0dHA6Ly93d3cudzMub3JnLzIwMDAvc3ZnIj48cmVjdCB3aWR0aD0iMjAwIiBoZWlnaHQ9IjEyMCIgZmlsbD0iIzY2NiIvPjxjaXJjbGUgY3g9IjEwMCIgY3k9IjYwIiByPSI0MCIgZmlsbD0iIzlhOWE5YSIvPjxwYXRoIGQ9Ik02NSA0NUw5NSA2MCIgc3Ryb2tlPSIjOWE5YTlhIiBzdHJva2Utd2lkdGg9IjUiLz48L3N2Zz4=';
		placeholder.alt = '视频预览不可用';

		container.appendChild(placeholder);
	}

	/**
	 * 创建配置面板
	 * @returns {HTMLElement} 配置面板元素
	 */
	createConfigPanel() {
		const panel = document.createElement('div');
		panel.className = 'config-panel';

		// 添加调节项
		panel.appendChild(this.createImageAdjustGroup());
		panel.appendChild(this.createExposureAdjustGroup());
		panel.appendChild(this.createWhiteBalanceAdjustGroup());
		panel.appendChild(this.createEnhanceGroup());

		return panel;
	}

	/**
	 * 创建图像调节组
	 * @returns {HTMLElement} 图像调节组元素
	 */
	createImageAdjustGroup() {
		const group = this.createGroupContainer('图像调节');

		// 添加模式下拉框
		group.appendChild(this.createModeDropdown('image'));

		// 添加各参数滑动条
		Object.entries(this.paramStateGUI.image).forEach(([key, config]) => {
			group.appendChild(this.createSliderControl(
				'image',				// 所属调节组
				key,					// 参数标识
				this.getParamLabel(key), // 参数中文标签
				config.value,		// 初始值
				config.min,			 // 最小值
				config.max,			 // 最大值
				config.unit			 // 单位
			));
		});

		return group;
	}

	/**
	 * 创建曝光调节组
	 * @returns {HTMLElement} 曝光调节组元素
	 */
	createExposureAdjustGroup() {
		const group = this.createGroupContainer('曝光调节');

		// 添加模式下拉框
		group.appendChild(this.createModeDropdown('exposure'));

		// 添加各参数滑动条
		Object.entries(this.paramStateGUI.exposure).forEach(([key, config]) => {
			group.appendChild(this.createSliderControl(
				'exposure',			// 所属调节组
				key,					 // 参数标识
				this.getParamLabel(key),// 参数中文标签
				config.value,			// 初始值
				config.min,			// 最小值
				config.max,			// 最大值
				config.unit			// 单位
			));
		});

		return group;
	}
	/**
	 * 创建白平衡调节组
	 * @returns {HTMLElement} 白平衡调节组元素
	 */
	createWhiteBalanceAdjustGroup() {
		const group = this.createGroupContainer('白平衡调节');

		// 添加模式下拉框
		group.appendChild(this.createModeDropdown('whiteBalance'));

		// 添加白平衡参数滑动条
		Object.entries(this.paramStateGUI.whiteBalance).forEach(([key, config]) => {
			group.appendChild(this.createSliderControl(
				'whiteBalance',		 // 所属调节组
				key,					// 参数标识
				this.getParamLabel(key), // 参数中文标签
				config.value,		// 初始值
				config.min,			 // 最小值
				config.max,			 // 最大值
				config.unit			 // 单位
			));
		});

		return group;
	}
	/**
	 * 创建图像增强组
	 * @returns {HTMLElement} 图像增强组元素
	 */
	createEnhanceGroup() {
		const group = this.createGroupContainer('图像增强');

		// 1. 空域降噪子项
		const nr2dSubGroup = document.createElement('div');
		nr2dSubGroup.className = 'enhance-subgroup';
		nr2dSubGroup.appendChild(this.createModeDropdown('nr2d', '空域降噪'));
		nr2dSubGroup.appendChild(this.createEnhanceLevelControl(
			'nr2d',
			'空域降噪强度',
			this.paramStateGUI.enhance.nr2d.level.value,
			this.paramStateGUI.enhance.nr2d.level.min,
			this.paramStateGUI.enhance.nr2d.level.max,
			this.paramStateGUI.enhance.nr2d.level.unit
		));
		group.appendChild(nr2dSubGroup);

		// 2. 时域降噪子项
		const nr3dSubGroup = document.createElement('div');
		nr3dSubGroup.className = 'enhance-subgroup';
		nr3dSubGroup.appendChild(this.createModeDropdown('nr3d', '时域降噪'));
		nr3dSubGroup.appendChild(this.createEnhanceLevelControl(
			'nr3d',
			'时域降噪强度',
			this.paramStateGUI.enhance.nr3d.level.value,
			this.paramStateGUI.enhance.nr3d.level.min,
			this.paramStateGUI.enhance.nr3d.level.max,
			this.paramStateGUI.enhance.nr3d.level.unit
		));
		group.appendChild(nr3dSubGroup);

		return group;
	}

	/**
	 * 创建增强Level调节滑动条
	 * @param {string} enhanceKey - 增强类型
	 * @param {string} label - 中文标签
	 * @param {number} value - 初始Level
	 * @param {number} min - 最小Level
	 * @param {number} max - 最大Level
	 * @param {string} unit - 单位
	 * @returns {HTMLElement} Level调节控件
	 */
	createEnhanceLevelControl(enhanceKey, label, value, min, max, unit) {
		const container = document.createElement('div');
		container.className = `slider-item ${enhanceKey}-param`;

		// 参数标题和数值显示
		const header = document.createElement('div');
		header.className = 'slider-header';

		const paramLabel = document.createElement('span');
		paramLabel.textContent = label;
		paramLabel.style.fontSize = '14px'; // 统一参数标签字体大小

		const paramValue = document.createElement('span');
		paramValue.className = 'param-value';
		paramValue.textContent = `${value}${unit}`;
		paramValue.style.fontSize = '14px'; // 统一参数值字体大小

		header.appendChild(paramLabel);
		header.appendChild(paramValue);

		// 滑动条
		const slider = document.createElement('input');
		slider.type = 'range';
		slider.className = `param-slider ${this.modeStateGUI[enhanceKey] === 'manual' ? 'enabled' : 'disabled'}`;
		slider.dataset.param = 'level';
		slider.dataset.enhance = enhanceKey;
		slider.min = min;
		slider.max = max;
		slider.value = value;
		slider.disabled = this.modeStateGUI[enhanceKey] !== 'manual';

		// 滑动条事件
		slider.addEventListener('input', (e) => {
			const currentLevel = parseInt(e.target.value);
			paramValue.textContent = `${currentLevel}${unit}`;
			if (this.modeStateGUI[enhanceKey] === 'manual') {
				this.applyEnhanceLevelChange(enhanceKey, currentLevel);
			}
		});

		container.appendChild(header);
		container.appendChild(slider);
		return container;
	}

	/**
	 * 创建调节组容器
	 * @param {string} title - 组标题
	 * @returns {HTMLElement} 组容器元素
	 */
	createGroupContainer(title) {
		const container = document.createElement('div');
		container.className = 'adjust-group';

		// 组标题
		const titleEl = document.createElement('div');
		titleEl.className = 'group-title';
		titleEl.innerHTML = `<span class="title-dot">●</span>${title}`;
		titleEl.style.fontSize = '15px'; // 组标题稍大一点但统一

		container.appendChild(titleEl);
		return container;
	}

	/**
	 * 创建模式下拉框（统一字体大小）
	 * @param {string} groupKey - 调节组标识
	 * @param {string} [prefixText=''] - 增强项的前缀文本
	 * @returns {HTMLElement} 模式下拉框容器
	 */
	createModeDropdown(groupKey, prefixText = '') {
		const container = document.createElement('div');
		container.className = 'mode-dropdown-container';

		// 标签 - 统一字体大小并增加间距
		const label = document.createElement('label');
		label.className = 'mode-label';
		label.textContent = `${prefixText ? prefixText + ' ' : ''}模式`;
		label.htmlFor = `${groupKey}-mode-select`;
		label.style.marginRight = '10px';
		label.style.minWidth = '80px';
		label.style.fontSize = '14px'; // 与参数标签字体大小一致

		// 下拉框 - 统一字体大小
		const select = document.createElement('select');
		select.id = `${groupKey}-mode-select`;
		select.className = 'mode-select';
		select.style.minWidth = '100px';
		select.style.fontSize = '14px'; // 与标签字体大小一致

		// 选项：自动和手动
		const autoOption = document.createElement('option');
		autoOption.value = 'auto';
		autoOption.textContent = '自动';

		const manualOption = document.createElement('option');
		manualOption.value = 'manual';
		manualOption.textContent = '手动';

		// 设置当前选中值
		if (this.modeStateGUI[groupKey] === 'manual') {
			manualOption.selected = true;
		} else {
			autoOption.selected = true;
		}

		// 添加选项并绑定事件
		select.appendChild(autoOption);
		select.appendChild(manualOption);
		select.addEventListener('change', (e) => {
			this.switchMode(groupKey, e.target.value);
		});

		container.appendChild(label);
		container.appendChild(select);
		return container;
	}

	/**
	 * 创建滑动条控制项
	 * @param {string} groupKey - 调节组标识
	 * @param {string} paramKey - 参数标识
	 * @param {string} label - 参数中文标签
	 * @param {number} value - 初始值
	 * @param {number} min - 最小值
	 * @param {number} max - 最大值
	 * @param {string} unit - 单位
	 * @returns {HTMLElement} 滑动条控制项元素
	 */
	createSliderControl(groupKey, paramKey, label, value, min, max, unit) {
		const container = document.createElement('div');
		container.className = `slider-item ${groupKey}-param`;

		// 参数标题和数值显示
		const header = document.createElement('div');
		header.className = 'slider-header';

		const paramLabel = document.createElement('span');
		paramLabel.textContent = label;
		paramLabel.style.fontSize = '14px'; // 统一参数标签字体大小

		const paramValue = document.createElement('span');
		paramValue.className = 'param-value';
		paramValue.textContent = `${value}${unit}`;
		paramValue.style.fontSize = '14px'; // 统一参数值字体大小

		header.appendChild(paramLabel);
		header.appendChild(paramValue);

		// 滑动条
		const slider = document.createElement('input');
		slider.type = 'range';
		slider.className = `param-slider ${this.modeStateGUI[groupKey] === 'manual' ? 'enabled' : 'disabled'}`;
		slider.dataset.param = paramKey;
		slider.dataset.group = groupKey;
		slider.min = min;
		slider.max = max;
		slider.value = value;
		slider.disabled = this.modeStateGUI[groupKey] !== 'manual';

		// 滑动条事件
		slider.addEventListener('input', (e) => {
			const currentValue = e.target.value;
			paramValue.textContent = `${currentValue}${unit}`;
			this.paramStateGUI[groupKey][paramKey].value = parseInt(currentValue);
			this.applyParamChange(groupKey, paramKey, currentValue);
		});

		container.appendChild(header);
		container.appendChild(slider);
		return container;
	}

	/**
	 * 创建对话框底部（返回按钮）
	 * @returns {HTMLElement} 底部元素
	 */
	createFooter() {
		const footer = document.createElement('div');
		footer.className = 'dialog-footer';

		const backBtn = document.createElement('button');
		backBtn.className = 'back-btn';
		backBtn.textContent = '返回';
		backBtn.style.fontSize = '14px'; // 按钮文字大小统一

		backBtn.addEventListener('click', () => {
			if (this.userCallbacks && typeof this.userCallbacks.onReturn === 'function') {
				this.userCallbacks.onReturn();
			}
			this.destroyDialog();
		});

		footer.appendChild(backBtn);
		return footer;
	}

	/**
	 * 销毁对话框
	 */
	destroyDialog() {
		if (this.dialog && this.dialog.parentElement) {
			document.body.removeChild(this.dialog);
		}
		// 重置所有属性
		this.destroyAndStopPlayer();

		this.stopAutoModeSyncTimer();

		this.dialog = null;
		this.videoElement = null;
		this.canvasVideoElement = null;
		this.pipelineChannel = null;
	}

	/**
	 * 获取参数的中文标签
	 * @param {string} paramKey - 参数标识
	 * @returns {string} 中文标签
	 */
	getParamLabel(paramKey) {
		const labels = {
			// 图像调节参数
			brightness: '亮度',
			contrast: '对比度',
			saturation: '饱和度',
			sharpness: '锐度',
			// 曝光调节参数
			exposureTime: '曝光时间',
			again: '模拟增益',
			dgain: '数字增益',
			// 白平衡参数
			redGain: '红色通道增益',
			blueGain: '蓝色通道增益',
			// 增强参数
			level: '降噪强度'
		};
		return labels[paramKey] || paramKey;
	}
	applyParamChange(groupKey, paramKey, value) {
		console.log(`应用参数变化 - 组: ${groupKey}, 参数: ${paramKey}, 值: ${value}`);
		const mode = this.modeStateGUI[groupKey];
		if (mode === 'auto') {
			console.warn(`${groupKey} 处于自动模式，不允许修改参数`);
			return;
		}
		// 保存旧参数用于后续可能的对比或回滚
		const oldParams = this.deepClone(this.paramStateGUI[groupKey]);

		// 转换值为数字类型（确保与参数定义一致）
		const numericValue = Number(value);
		if (isNaN(numericValue)) {
			console.error(`无效的参数值: ${value}，必须是数字`);
			return;
		}
		if (this.paramStateGUI[groupKey] && this.paramStateGUI[groupKey][paramKey]) {
			this.paramStateGUI[groupKey][paramKey].value = numericValue;
			console.log(`已更新普通组参数: ${groupKey}.${paramKey}=${numericValue}`);
		} else {
			console.error(`普通组参数不存在: ${groupKey}.${paramKey}`);
		}
		this.syncParamToDevice(groupKey, paramKey, oldParams, mode);
	}

	/**
	 * 应用普通参数组模式变更（图像/曝光/白平衡）
	 * @param {string} groupKey - 调节组标识（image/exposure/whiteBalance）
	 * @param {string} mode - 目标模式（auto/manual）
	 */
	applyModeChange(groupKey, mode) {
		console.log(`参数调节模式切换 - 组: ${groupKey}, 模式: ${mode === 'auto' ? '自动' : '手动'}`);
		const oldMode = this.modeStateGUI[groupKey];
		const oldParams = this.deepClone(this.paramStateGUI[groupKey]);

		this.modeStateGUI[groupKey] = mode;

		// 确认设备参数源存在（容错处理）
		if (!this.paramStateDevice[groupKey]) {
			console.warn(`设备参数中无此组: ${groupKey}（对应GUI组: ${groupKey}）`);
			return;
		}

		// 根据模式选择设备参数的子源（auto取state，manual取manual）
		const paramSubSource = mode === 'manual'
			? this.paramStateDevice[groupKey][`manual`]
			: this.paramStateDevice[groupKey][`state`];

		// 全量同步当前组所有参数值到GUI
		Object.keys(this.paramStateGUI[groupKey]).forEach(paramKey => {
			// 从设备参数源中取对应值，无值则保留GUI当前值（避免显示异常）
			this.paramStateGUI[groupKey][paramKey].value =
				paramSubSource[paramKey] !== undefined
					? paramSubSource[paramKey]
					: this.paramStateGUI[groupKey][paramKey].value;
		});

		this.syncParamToDevice(groupKey, "mode", oldParams, oldMode);

		// 4. 全量刷新当前组的滑动条和数值显示
		this.refreshGroupUIControls(groupKey);
	}


	/**
	 * 应用增强参数组模式变更（空域降噪/时域降噪）
	 * @param {string} enhanceKey - 增强类型（nr2d/nr3d）
	 * @param {string} mode - 目标模式（auto/manual）
	 * @param {number} level - 当前GUI中的Level值（用于容错）
	 */
	applyEnhanceModeChange(enhanceKey, mode, level) {
		const enhanceType = enhanceKey === 'nr2d' ? '空域降噪' : '时域降噪';
		console.log(`应用增强模式 - 类型: ${enhanceType}, 模式: ${mode === 'auto' ? '自动' : '手动'}, Level: ${mode === 'manual' ? level : '自动'}`);

		const oldMode = this.modeStateGUI[enhanceKey];
		const oldParams = this.deepClone(this.paramStateGUI.enhance[enhanceKey]);

		// 1. 更新 modeStateGUI 对应的增强组模式
		this.modeStateGUI[enhanceKey] = mode;

		// 2. 以 this.paramStateDevice 为完整参数源，同步Level值到 paramStateGUI
		// 确认设备参数源存在（容错处理）
		if (!this.paramStateDevice[enhanceKey]) {
			console.warn(`设备参数中无此增强组: ${enhanceKey}（对应GUI组: ${enhanceKey}）`);
			// 无设备参数时，保留GUI当前Level值
			this.paramStateGUI.enhance[enhanceKey].level.value = level;
			return;
		}

		// 根据模式选择设备参数的子源（auto取state，manual取manual）
		const paramSubSource = mode === 'manual'
			? this.paramStateDevice[enhanceKey].manual
			: this.paramStateDevice[enhanceKey].state;

		// 同步Level值到GUI（优先设备值，无设备值则用当前GUI值）
		this.paramStateGUI.enhance[enhanceKey].level.value =
			paramSubSource.level !== undefined
				? paramSubSource.level
				: level;

		// 3. 调用硬件API设置设备增强模式（传递完整设备参数源）
		this.syncParamToDevice(enhanceKey, "mode", oldParams, oldMode);

		// 4. 全量刷新当前增强组的滑动条和数值显示
		this.refreshGroupUIControls(enhanceKey, 'enhance');
	}


	/**
	 * 应用增强Level变更
	 * @param {string} enhanceKey - 增强类型
	 * @param {number} level - Level值
	 */
	applyEnhanceLevelChange(enhanceKey, level) {
		const enhanceType = enhanceKey === 'nr2d' ? '空域降噪' : '时域降噪';
		console.log(`应用增强Level变化 - 类型: ${enhanceType}, Level值: ${level}`);

		// 检查当前模式，自动模式下不允许修改
		const mode = this.modeStateGUI[enhanceKey];
		if (mode === 'auto') {
			console.warn(`${enhanceType}处于自动模式，不允许修改Level参数`);
			return;
		}

		// 保存旧参数用于对比
		const oldParams = this.deepClone(this.paramStateGUI.enhance[enhanceKey]);
		// 转换Level为数字并验证有效性
		const numericLevel = Number(level);
		if (isNaN(numericLevel)) {
			console.error(`无效的Level值: ${level}，必须是数字`);
			return;
		}

		// 验证参数路径是否存在
		if (this.paramStateGUI.enhance && this.paramStateGUI.enhance[enhanceKey] &&
			this.paramStateGUI.enhance[enhanceKey].level) {
			// 更新增强组Level参数
			this.paramStateGUI.enhance[enhanceKey].level.value = numericLevel;
			console.log(`已更新增强组Level: ${enhanceKey}.level=${numericLevel}, old level is ${oldParams.level.value}`);
			// 同步参数到设备（与普通组保持一致的同步逻辑）
			this.syncParamToDevice(enhanceKey, 'level', oldParams, mode);
		} else {
			console.error(`增强组参数不存在: ${enhanceKey}.level`);
		}
	}

	/**
	 * 切换模式（自动/手动）
	 * @param {string} groupKey - 调节组标识
	 * @param {string} mode - 目标模式（auto/manual）
	 */
	switchMode(groupKey, mode) {
		// 处理增强组模式切换
		if (groupKey === 'nr2d' || groupKey === 'nr3d') {
			const enhanceType = groupKey === 'nr2d' ? '空域降噪' : '时域降噪';
			const currentLevel = this.paramStateGUI.enhance[groupKey].level.value;
			console.log(`增强模式切换 - 类型: ${enhanceType}, 模式: ${mode === 'auto' ? '自动' : '手动'}, 当前Level: ${currentLevel}`);
			this.applyEnhanceModeChange(groupKey, mode, currentLevel);
		} else {
			this.applyModeChange(groupKey, mode);
		}

		// 更新滑动条状态
		this.updateSliderStates(groupKey, mode === 'manual');
	}

	/**
	 * 更新滑动条状态（启用/禁用）
	 * @param {string} groupKey - 调节组标识
	 * @param {boolean} enabled - 是否启用
	 */
	updateSliderStates(groupKey, enabled) {
		const sliders = document.querySelectorAll(`.${groupKey}-param .param-slider`);
		sliders.forEach(slider => {
			slider.classList.toggle('enabled', enabled);
			slider.classList.toggle('disabled', !enabled);
			slider.disabled = !enabled;
			// 增强组自动模式触发自动调节
			if ((groupKey === 'nr2d' || groupKey === 'nr3d') && !enabled) {
				const currentLevel = this.paramStateGUI.enhance[groupKey].level.value;
				this.applyEnhanceModeChange(groupKey, 'auto', currentLevel);
			}
		});
	}
	 /**
	 * 同步参数到设备，仅包含参数当前值、模式及历史信息
	 * @param {string} groupKey - 组名称（GUI组标识）
	 * @param {string} key - 当前配置项的key值
	 * @param {Object} oldParams - 历史参数（this.paramStateGUI中对应组的完整参数）
	 * @param {string} oldMode - 历史模式
	 */
	syncParamToDevice(groupKey, key, oldParams, oldMode) {
		// 1. 验证必要参数是否存在
		if (!groupKey || !key || oldParams === undefined || oldMode === undefined) {
			console.error('syncParamToDevice缺少必要参数', { groupKey, key, oldParams, oldMode });
			return;
		}

		// 2. 获取当前组的所有参数（从paramStateGUI）
		const currentGroupSource = this.paramStateGUI[groupKey] ||
								(this.paramStateGUI.enhance && this.paramStateGUI.enhance[groupKey]);

		if (!currentGroupSource) {
			console.error(`未找到组${groupKey}的当前参数`);
			return;
		}

		// 3. 提取参数的当前值（仅保留value，剔除min/max/unit等）
		const currentParams = Object.keys(currentGroupSource).reduce((acc, paramKey) => {
			// 处理普通参数（如brightness）和增强组参数（如level）的结构一致性
			acc[paramKey] = currentGroupSource[paramKey].value;
			return acc;
		}, {});

		// 4. 提取历史参数的当前值（仅保留value）
		const oldParamsSimplified = Object.keys(oldParams).reduce((acc, paramKey) => {
			acc[paramKey] = oldParams[paramKey].value;
			return acc;
		}, {});

		// 5. 获取当前组的模式
		const currentMode = this.modeStateGUI[groupKey];

		// 6. 构建上报参数结构（仅包含当前值）
		const reportData = {
			video_id: Number(this.pipelineChannel),
			params: {
				valueKey: key,						// 当前配置项的key值
				groupKey: groupKey,				// 组名字
				configs: {						// 当前组的所有参数值和当前模式
					mode: currentMode,			// 附加当前模式
					...currentParams,			// 仅包含各参数的value
				},
				configsOld: {					// 历史参数值和历史模式
					mode: oldMode,				// 附加历史模式
					...oldParamsSimplified,		 // 仅包含各参数的历史value
				}
			}

		};

		// 7. 执行上报（通过用户回调）
		if (this.userCallbacks && typeof this.userCallbacks.onSetISPParam === 'function') {
			this.userCallbacks.onSetISPParam(reportData);
		} else {
			console.warn('未设置onSetISPParam回调，参数未实际上报', reportData);
		}
	}
	deepClone(obj) {
		if (obj === null || typeof obj !== 'object') return obj;
		const clone = Array.isArray(obj) ? [] : {};
		for (const key in obj) {
			if (obj.hasOwnProperty(key)) {
			clone[key] = this.deepClone(obj[key]);
			}
		}
		return clone;
	}
}
export default ImageQualityControler;