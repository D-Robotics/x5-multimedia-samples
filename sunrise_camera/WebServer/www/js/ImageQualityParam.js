class ImageQualityParam {
	/**
	 * 构造函数，初始化所有参数
	 */
	constructor() {
		// 图像参数
		this.image = {
			mode: "auto", // 模式：auto/manual
			manual: {
				brightness: 0,
				contrast: 0,
				saturation: 0,
				sharpness: 0
			},
			state: {
				brightness: 0,
				contrast: 0,
				saturation: 0,
				sharpness: 0
			}
		};

		// 曝光参数
		this.exposure = {
			mode: "auto",
			manual: {
				exposureTime: 0,
				again: 0,
				dgain: 0
			},
			state: {
				exposureTime: 0,
				again: 0,
				dgain: 0
			}
		};

		// 白平衡参数
		this.whiteBalance = {
			mode: "auto",
			manual: {
				redGain: 0,
				blueGain: 0
			},
			state: {
				redGain: 0,
				blueGain: 0
			}
		};

		// 2DNR参数（空域降噪）
		this.nr2d = {
			mode: "auto",
			manual: {
				level: 0
			},
			state: {
				level: 0
			}
		};

		// 3DNR参数（时域降噪）
		this.nr3d = {
			mode: "auto",
			manual: {
				level: 0
			},
			state: {
				level: 0
			}
		};

		// 【新增】定义合法的参数组列表（用于校验输入的组名）
		this.VALID_GROUPS = ['image', 'exposure', 'whiteBalance', 'nr2d', 'nr3d'];
	}

	/**
	 * 从JSON对象初始化当前实例
	 * @param {Object} json - 包含参数的JSON对象
	 * @returns {ImageQualityParam} 当前实例
	 */
	fromJSON(json) {
		if (!json) return this;

		// 处理图像参数
		if (json.image) {
			this.image.mode = json.image.mode || "auto";
			if (json.image.manual) {
				this.image.manual = {
					brightness: json.image.manual.brightness || 0,
					contrast: json.image.manual.contrast || 0,
					saturation: json.image.manual.saturation || 0,
					sharpness: json.image.manual.sharpness || 0
				};
			}
			if (json.image.state) {
				this.image.state = {
					brightness: json.image.state.brightness || 0,
					contrast: json.image.state.contrast || 0,
					saturation: json.image.state.saturation || 0,
					sharpness: json.image.state.sharpness || 0
				};
			}
		}

		// 处理曝光参数
		if (json.exposure) {
			this.exposure.mode = json.exposure.mode || "auto";
			if (json.exposure.manual) {
				this.exposure.manual = {
					exposureTime: json.exposure.manual.exposureTime || 0,
					again: json.exposure.manual.again || 0,
					dgain: json.exposure.manual.dgain || 0
				};
			}
			if (json.exposure.state) {
				this.exposure.state = {
					exposureTime: json.exposure.state.exposureTime || 0,
					again: json.exposure.state.again || 0,
					dgain: json.exposure.state.dgain || 0
				};
			}
		}

		// 处理白平衡参数
		if (json.whiteBalance) {
			this.whiteBalance.mode = json.whiteBalance.mode || "auto";
			if (json.whiteBalance.manual) {
				this.whiteBalance.manual = {
					redGain: json.whiteBalance.manual.redGain || 0,
					blueGain: json.whiteBalance.manual.blueGain || 0
				};
			}
			if (json.whiteBalance.state) {
				this.whiteBalance.state = {
					redGain: json.whiteBalance.state.redGain || 0,
					blueGain: json.whiteBalance.state.blueGain || 0
				};
			}
		}

		// 处理2DNR参数
		if (json.nr2d) {
			this.nr2d.mode = json.nr2d.mode || "auto";
			if (json.nr2d.manual) {
				this.nr2d.manual = {
					level: json.nr2d.manual.level || 0
				};
			}
			if (json.nr2d.state) {
				this.nr2d.state = {
					level: json.nr2d.state.level || 0
				};
			}
		}

		// 处理3DNR参数
		if (json.nr3d) {
			this.nr3d.mode = json.nr3d.mode || "auto";
			if (json.nr3d.manual) {
				this.nr3d.manual = {
					level: json.nr3d.manual.level || 0
				};
			}
			if (json.nr3d.state) {
				this.nr3d.state = {
					level: json.nr3d.state.level || 0
				};
			}
		}

		return this;
	}

	/**
	 * 从JSON字符串初始化当前实例
	 * @param {string} jsonStr - JSON字符串
	 * @returns {ImageQualityParam} 当前实例
	 */
	fromJSONString(jsonStr) {
		try {
			const json = JSON.parse(jsonStr);
			return this.fromJSON(json);
		} catch (e) {
			console.error("Failed to parse JSON string:", e);
			return this;
		}
	}

	fromGroupJSON(group, paramsJsons, needUpdateState = false) {
		try {
			if(group == "image"){
				this.image.mode = paramsJsons.mode;
				if(this.image.mode === "manaul"){
					this.image.manual = {
						brightness: paramsJsons.brightness ,
						contrast: paramsJsons.contrast ,
						saturation: paramsJsons.saturation ,
						sharpness: paramsJsons.sharpness
					};
				}else if(needUpdateState){
					this.image.state = {
						brightness: paramsJsons.brightness ,
						contrast: paramsJsons.contrast ,
						saturation: paramsJsons.saturation ,
						sharpness: paramsJsons.sharpness
					};
				}
			}else if(group == "exposure"){
				this.exposure.mode = paramsJsons.mode;
				if(this.image.mode === "manaul"){
					this.exposure.manual = {
						exposureTime: paramsJsons.exposureTime ,
						again: paramsJsons.again ,
						dgain: paramsJsons.dgain
					};
				}else if(needUpdateState){
					this.exposure.state = {
						exposureTime: paramsJsons.exposureTime ,
						again: paramsJsons.again ,
						dgain: paramsJsons.dgain
					};
				}
			}else if(group == "whiteBalance"){
				this.whiteBalance.mode = paramsJsons.mode;
				if(this.image.mode === "manaul"){
					this.whiteBalance.manual = {
						redGain: paramsJsons.redGain ,
						blueGain: paramsJsons.blueGain
					};
				}else if(needUpdateState){
					this.whiteBalance.state = {
						redGain: paramsJsons.redGain ,
						blueGain: paramsJsons.blueGain
					};
				}

			}else if(group == "nr2d"){
				this.nr2d.mode = paramsJsons.mode;
				if(this.image.mode === "manaul"){
					this.nr2d.manual = {
						level: paramsJsons.level
					};
				}else if(needUpdateState){
				   this.nr2d.state = {
						level: paramsJsons.level
					};
				}
			}else if(group == "nr3d"){
				this.nr3d.mode = paramsJsons.mode;
				if(this.image.mode === "manaul"){
					this.nr3d.manual = {
						level: paramsJsons.level
					};
				}else if(needUpdateState){
					this.nr3d.state = {
						level: paramsJsons.level
					};
				}
			}
		} catch (e) {
			console.error("Failed to parse JSON string:", e);
			return this;
		}
	}

	/**
	 * 转换为完整的JSON对象（原功能保留）
	 * @returns {Object} 包含所有参数的JSON对象
	 */
	toJSON() {
		return {
			image: {
				mode: this.image.mode,
				manual: this.image.manual,
				state: this.image.state
			},
			exposure: {
				mode: this.exposure.mode,
				manual: this.exposure.manual,
				state: this.exposure.state
			},
			whiteBalance: {
				mode: this.whiteBalance.mode,
				manual: this.whiteBalance.manual,
				state: this.whiteBalance.state
			},
			nr2d: {
				mode: this.nr2d.mode,
				manual: this.nr2d.manual,
				state: this.nr2d.state
			},
			nr3d: {
				mode: this.nr3d.mode,
				manual: this.nr3d.manual,
				state: this.nr3d.state
			}
		};
	}

	/**
	 * 转换为完整的JSON字符串（原功能保留）
	 * @returns {string} JSON字符串
	 */
	toJSONString() {
		try {
			return JSON.stringify(this.toJSON(), null, 2);
		} catch (e) {
			console.error("Failed to stringify to JSON:", e);
			return "{}";
		}
	}

	/**
	 * 【新增】根据组名，输出指定参数组的JSON字符串
	 * @param {string} groupName - 目标参数组名称（如 'image'/'exposure'/'whiteBalance'/'nr2d'/'nr3d'）
	 * @param {number} [space=2] - JSON格式化缩进空格数（默认2，优化可读性）
	 * @returns {string} 指定组的JSON字符串（若组名无效，返回空对象JSON）
	 */
	toGroupJSONString(groupName, space = 2) {
		// 1. 校验组名是否合法
		if (!this.VALID_GROUPS.includes(groupName)) {
			console.warn(`Invalid group name: ${groupName}. Valid groups are: ${this.VALID_GROUPS.join(', ')}`);
			return JSON.stringify({}, null, space); // 返回空对象，避免后续报错
		}

		// 2. 提取指定组的参数（保持与完整toJSON一致的结构）
		const groupData = {
			[groupName]: this[groupName] // 动态匹配组数据（如 groupName='image' 则取 this.image）
		};

		// 3. 序列化为JSON字符串（处理可能的循环引用/序列化错误）
		try {
			return JSON.stringify(groupData, null, space);
		} catch (e) {
			console.error(`Failed to stringify group ${groupName} to JSON:`, e);
			return JSON.stringify({}, null, space);
		}
	}

	/**
	 * 设置指定参数组的模式
	 * @param {string} group - 参数组名称(image/exposure/whiteBalance/nr2d/nr3d)
	 * @param {string} mode - 模式(auto/manual)
	 * @returns {boolean} 设置是否成功
	 */
	setMode(group, mode) {
		if (!this[group] || !["auto", "manual"].includes(mode)) {
			console.error(`Invalid group ${group} or mode ${mode}`);
			return false;
		}
		this[group].mode = mode;
		return true;
	}

	/**
	 * 获取指定参数组的模式
	 * @param {string} group - 参数组名称
	 * @returns {string} 当前模式
	 */
	getMode(group) {
		if (!this[group]) {
			console.error(`Invalid group ${group}`);
			return "auto";
		}
		return this[group].mode;
	}
}

export default ImageQualityParam;
