document.write("<script type='text/javascript' src='js/websocket.js'></script>");
document.write("<script type='text/javascript' src='js/wfs/wfs.js'></script>");
document.write("<script type='text/javascript' src='js/jquery.min.js'></script>");
document.write("<script type='text/javascript' src='js/imagenet_id_to_classname.js'></script>");

var g_solution_configs = "";
var g_current_layout = 2;

// 定义一个队列数组
var g_alog_result_queue_array = [];

// json中各个字段的中文名，字段类型，以及可使用的数据
const g_solution_fields = {
	"pipeline_count": {
		chinese_name: "视频通路数",
		type: "maxlimit",
		options: "max_pipeline_count"
	},
	"sensor": {
		chinese_name: "Sensor型号",
		type: "stringlist",
		options: "sensor_list" // Placeholder for sensor options
	},
	"encode_type": {
		chinese_name: "编码类型",
		type: "stringlist",
		options: "codec_type_list"
	},
	"encode_bitrate": {
		chinese_name: "编码码率",
		type: "intarray",
		options: "encode_bit_rate_list"
	},
	"model": {
		chinese_name: "算法模型",
		type: "stringlist",
		options: "model_list" // Placeholder for model options
	},
	"stream": {
		chinese_name: "视频数据流",
		type: "text",
		value: "../test_data/1080P_test.h264" // Placeholder for stream value
	},
	"decode_type": {
		chinese_name: "解码类型",
		type: "stringlist",
		options: "codec_type_list"
	},
	"decode_width": {
		chinese_name: "解码宽度",
		type: "int",
		value: 1920 // Placeholder for decode_width value
	},
	"decode_height": {
		chinese_name: "解码高度",
		type: "int",
		value: 1080 // Placeholder for decode_height value
	},
	"decode_frame_rate": {
		chinese_name: "解码帧率",
		type: "int",
		value: 30 // Placeholder for decode_frame_rate value
	},
	"encode_width": {
		chinese_name: "编码宽度",
		type: "int",
		value: 1920 // Placeholder for encode_width value
	},
	"encode_height": {
		chinese_name: "编码高度",
		type: "int",
		value: 1080 // Placeholder for encode_height value
	},
	"encode_frame_rate": {
		chinese_name: "编码帧率",
		type: "int",
		value: 30 // Placeholder for encode_frame_rate value
	}
};

const REQUEST_TYPES = {
	APP_SWITCH: 1,
	SNAPSHOT: 2,
	START_STREAM: 3,
	STOP_STREAM: 4,
	SYNC_TIME: 5,
	SET_BITRATE: 6,
	GET_CONFIG: 7,
	SAVE_CONFIGS: 8,
	RECOVERY_CONFIGS: 9,
	ALOG_RESULT: 10
};

function update_solution_status(solutions_config) {
	// 获取状态显示的DOM元素
	const solution_status = document.getElementById("solution_status");

	// 初始化状态文本
	let status_txt = "<h4>当前方案配置：</h4>";

	// 检查当前方案类型
	const solution_name = solutions_config["solution_name"];
	if (solution_name === 'cam_solution') {
		// 如果是智能摄像机方案
		status_txt += "<strong>智能摄像机：</strong></br>";

		// 获取摄像机方案的信息
		const cam_solution = solutions_config["cam_solution"];
		const pipeline_count = cam_solution["pipeline_count"];

		// 更新状态文本，显示启用的视频路数
		status_txt += `- 启用 ${pipeline_count} 路视频</br>`;

		// 列出启用的传感器型号和算法模型
		status_txt += "<strong>启用的Sensor型号和算法模型：</strong></br>";
		for (let i = 0; i < pipeline_count; i++) {
			const sensor_model = cam_solution["cam_vpp"][i]["sensor"];
			const algorithm_model = cam_solution["cam_vpp"][i]["model"];
			status_txt += `- 第 ${i+1} 路:`;
			status_txt += `<ul>`;
			status_txt += `<li>Sensor型号：${sensor_model}</li>`;
			status_txt += `<li>算法：${algorithm_model}</li>`;
			status_txt += `</ul>`;
		}
		status_txt += "<strong>方案框图（点击放大）</strong></br>";
		status_txt += `<img id="solution_image" src="image/camera-slt.jpg" style="display: block;max-width:100%; max-height:100%" />`;
	} else if (solution_name === 'box_solution') {
		// 如果是智能分析盒方案
		status_txt += `<strong>智能分析盒：</strong></br>`;

		// 获取分析盒方案的信息
		const box_solution = solutions_config["box_solution"];
		const pipeline_count = box_solution["pipeline_count"];

		// 更新状态文本，显示启用的视频路数
		status_txt += `- 启用 ${pipeline_count} 路视频</br>`;

		// 列出编解码的基本分辨率信息和算法模型
		status_txt += "<strong>编解码和算法模型：</strong></br>";
		for (let i = 0; i < pipeline_count; i++) {
			const decode_resolution = `${box_solution["box_vpp"][i]["decode_width"]}
				x${box_solution["box_vpp"][i]["decode_height"]}@
				${box_solution["box_vpp"][i]["decode_frame_rate"]}fps`;

			const encode_resolution = `${box_solution["box_vpp"][i]["encode_width"]}
				x${box_solution["box_vpp"][i]["encode_height"]}@
				${box_solution["box_vpp"][i]["encode_frame_rate"]}fps`;

			const algorithm_model = box_solution["box_vpp"][i]["model"];
			status_txt += `- 第 ${i+1} 路:`;
			status_txt += `<ul>`;
			status_txt += `<li>解码：${decode_resolution}</li>`;
			status_txt += `<li>编码：${encode_resolution}</li>`;
			status_txt += `<li>算法：${algorithm_model}</li>`;
			status_txt += `</ul>`;
		}
		status_txt += "<strong>方案框图（点击放大）</strong></br>";
		status_txt += `<img id="solution_image" src="image/box-slt.jpg" style="display: block;max-width:100%; max-height:100%" />`;
	}

	// 更新状态信息到页面上
	solution_status.innerHTML = status_txt;

	// 添加类以靠左对齐状态消息
	solution_status.classList.add('left-align');

	// 添加点击事件监听器到图片
	document.getElementById("solution_image").addEventListener("click", function() {
		// 创建模态框
		var modal = document.createElement("div");
		modal.style.position = "fixed";
		modal.style.top = "0";
		modal.style.left = "0";
		modal.style.width = "100%";
		modal.style.height = "100%";
		modal.style.backgroundColor = "rgba(0,0,0,0.7)";
		modal.style.zIndex = "1000";
		modal.style.display = "flex";
		modal.style.alignItems = "flex-start";
		modal.style.justifyContent = "center";

		// 创建放大后的图片
		var enlargedImage = document.createElement("img");
		enlargedImage.src = this.src; // 使用点击的图片的 src
		enlargedImage.style.maxWidth = "90%";
		enlargedImage.style.maxHeight = "90%";

		// 添加放大后的图片到模态框中
		modal.appendChild(enlargedImage);

		// 添加关闭按钮到模态框
		var closeButton = document.createElement("button");
		closeButton.textContent = "关闭";
		closeButton.style.marginLeft = "10px"; // 调整关闭按钮的左外边距，使其位于图片右侧
		closeButton.style.padding = "5px 10px";
		closeButton.style.border = "none";
		closeButton.style.backgroundColor = "#ffffff";
		closeButton.style.cursor = "pointer";
		closeButton.addEventListener("click", function() {
			modal.remove(); // 关闭模态框
		});
		modal.appendChild(closeButton);

		// 将模态框添加到页面中
		document.body.appendChild(modal);
	});

}

// 定义一个数组来存储所有视频元素
var videos = [];

// 定义处理视频帧的函数
function processVideoFrame(index) {
	var video = videos[index-1];

	if (video.buffered.length) {
		var end = video.buffered.end(0); //获取当前buffered值
		var diff = end - video.currentTime; //获取buffered与currentTime的差值
		// 差值小于0.3s时根据1倍速进行播放
		if (diff <= 0.3) {
			video.playbackRate = 1;
		}
		// 差值大于0.3s小于5s根据1.2倍速进行播放
		if (diff < 5 && diff > 0.3) {
			video.playbackRate = 1.2;
		}
		if (diff >= 5) {
			console.log("video buffer diff: " + diff);
			//如果差值大于等于5 手动跳帧 这里可根据自身需求来定
			video.currentTime = video.buffered.end(0); //手动跳帧
		}
	}

	return function(timestamp) {
		// 获取当前视频播放时间
		var currentVideoTime = video.currentTime; // 单位秒， 这个值会跟随 video.playbackRate的设置按倍数增加，作为时间差值会有一点问题
		var closestElement = null;
		var closestDiff = Infinity;
		// 定义误差时间
		var errorTime = 100*1000;

		// console.log("pipeline index:", index, "socket.stream_first_timestamp:",
		// 	socket.stream_first_timestamp[index],
		// 	"currentVideoTime", currentVideoTime,
		// 	"timestamp:", timestamp,
		// 	"video.playbackRate:", video.playbackRate);

		var targetTimestamp = (currentVideoTime * 1000000) + parseFloat(socket.stream_first_timestamp[index]); //单位是微秒
		var remainingOptionsBefore = g_alog_result_queue_array[index].length; // 遍历前的剩余选项数

		for (var i = 0; i < g_alog_result_queue_array[index].length; i++) {
			var currElement = g_alog_result_queue_array[index][i];
			var currTimestamp = currElement.timestamp;
			var timeDiff = Math.abs(currTimestamp - targetTimestamp);

			// console.log("pipeline index:", index, "Element:", i, "currTimestamp:", currTimestamp, "targetTimestamp:",
			// 	targetTimestamp, "Time difference:", currTimestamp - targetTimestamp, "timeDiff:", timeDiff);

			// 如果当前元素的时间戳与目标时间戳相等，则直接选择该元素
			if (timeDiff == 0) {
				closestElement = currElement;
				break; // 跳出循环，因为已经找到了匹配的元素
			}

			// 如果当前元素的时间戳大于目标时间戳，则选择上一个元素（如果存在）作为最接近的元素
			if (currTimestamp > targetTimestamp) {
					// 如果是队列的第一个元素，则直接选择该元素
					if (i === 0) {
						// 如果当前元素的时间戳与目标时间戳的差值小于100ms，并且比之前的差值更小，则更新最接近的元素和差值
						// 视频和算法的时间戳误差小于100ms时，认为匹配成功
						if (timeDiff <= errorTime)
							closestElement = currElement;
					} else {
						// 否则选择当前元素与前一个元素中与目标时间戳更接近的元素
						var prevTimestamp = g_alog_result_queue_array[index][i - 1].timestamp;
						var prevDiff = Math.abs(prevTimestamp - targetTimestamp);
						closestDiff = (timeDiff < prevDiff) ? timeDiff : prevDiff;
						if (closestDiff <= errorTime)
							closestElement = (timeDiff < prevDiff) ? currElement : g_alog_result_queue_array[index][i - 1];
					}
					break; // 跳出循环，因为已经找到了最接近的元素
			}

			// 如果遍历到了队列的最后一个元素，选择该元素作为最接近的元素
			if (i === g_alog_result_queue_array[index].length - 1) {
				closestElement = g_alog_result_queue_array[index][i];
			}
		}

		// 渲染算法结果
		if (closestElement) {
			// console.log("pipeline index:", index, "targetTimestamp:", targetTimestamp, "Closest diff:", closestDiff,
			// 	"Closest element:", closestElement);
			if (closestElement.classification_result) {
				show_classification_result(closestElement.pipeline, closestElement.classification_result);
			}
			if (closestElement.detection_result) {
				draw_detection_result(closestElement.pipeline, closestElement.detection_result);
			}
			// 删除已经处理过的元素
			var closestIndex = g_alog_result_queue_array[index].indexOf(closestElement);
			g_alog_result_queue_array[index].splice(closestIndex, 1);
		}

		var remainingOptionsAfter = g_alog_result_queue_array[index].length; // 遍历后的剩余选项数
		// console.log("Remaining options before:", remainingOptionsBefore, "Remaining options after:", remainingOptionsAfter);

		// remainingOptionsAfter 大于 30, 可能存在算法结果过时的情况，遍历，把过时超过一定时间的记录删除
		if (remainingOptionsAfter > 30) {
			for (var i = 0; i < g_alog_result_queue_array[index].length; i++) {
				var currElement = g_alog_result_queue_array[index][i];
				var currTimestamp = currElement.timestamp;
				var timeDiff = targetTimestamp - currTimestamp;
				if (timeDiff > 100 * 1000) {
					g_alog_result_queue_array[index].splice(i, 1);
				}
			}
		}
		// 使用requestAnimationFrame()递归调用自身，以便在下一帧更新时执行
		requestAnimationFrame(processVideoFrame(index));
	};
}

// 当视频准备就绪时
function handleLoadedData(index) {
	return function() {
		// 启动帧更新循环
		requestAnimationFrame(processVideoFrame(index));
	};
}

function open_solution_info(solution) {
	// 根据所选的解决方案，打开相应的HTML页面
	if (solution === "cam_solution") {
		window.open("cam_solution_info.html", "_blank");
	} else if (solution === "box_solution") {
		window.open("box_solution_info.html", "_blank");
	}
}

function adjust_layout(num_videos) {
	const layouts = document.querySelectorAll('.layout');
	layouts.forEach(layout => {
		layout.style.display = 'none';
	});

	if (num_videos === 1) {
		document.getElementById('layout1').style.display = 'block';
	} else if (num_videos === 2) {
		document.getElementById('layout2').style.display = 'flex';
	}else {
		document.getElementById(`layout${num_videos}`).style.display = 'grid';
	}

	// 循环遍历每一个视频元素
	// 添加事件监听器
	for (var i = 1; i <= num_videos; i++) {
		var video = document.getElementById(`video${num_videos}_${i}`);
		if (video) {
			// 将视频元素添加到数组中
			videos.push(video);
			// 监听视频准备就绪事件
			video.addEventListener('loadeddata', handleLoadedData(i));
		}
	}
}

function render_label_name(solutions_config, itemKey, uniqueId, vpp_config) {
	const field = g_solution_fields[itemKey];
	const label = field ? `${field.chinese_name}（${itemKey}）` : itemKey;
	let html = `<li><span>${label}</span>：`;

	const hardware_capability = solutions_config["hardware_capability"];

	if (field) {
		if (field.type === 'stringlist') {
			html += `<select id="${uniqueId}" class="form-control-sm">`;
			const options = hardware_capability[field.options].split('/');
			options.forEach(option => {
				html += `<option value="${option}" ${vpp_config[itemKey] === option ? 'selected' : ''}>${option}</option>`;
			});
			html += `</select>`;
		} else if (field.type === 'intarray') {
			html += `<select id="${uniqueId}" class="form-control-sm">`;
			const options = hardware_capability[field.options];
			options.forEach(option => {
				if (option > 0)
					html += `<option value="${option}" ${vpp_config[itemKey] === option ? 'selected' : ''}>${option}Kbps</option>`;
			});
			html += `</select>`;
		} else if (field.type === 'text') {
			const textValue = vpp_config[itemKey] && vpp_config[itemKey] !== '0' ? vpp_config[itemKey] : field.value;
			html += `<input type="text" id="${uniqueId}" value="${textValue}">`;
		} else if (field.type === 'int') {
			const textValue = vpp_config[itemKey] && vpp_config[itemKey] !== '0' ? vpp_config[itemKey] : field.value;
			html += `<input type="number" id="${uniqueId}" value="${textValue}" step="1">`;
		}
	} else {
		html += `<input type="text" id="${uniqueId}" value="${vpp_config[itemKey]}">`;
	}

	html += `</li>`;
	return html;
}

// 将 JSON 渲染到 HTML 的函数
function render_json_to_html(solutions_config) {
	const container = document.getElementById('solutionConfig');
	let html = '';

	console.log("solutions_config");

	const solution_name = solutions_config["solution_name"];
	const hardware_capability = solutions_config["hardware_capability"];

	html += `<h2>设备信息</h2>`
	html += `<span style="white-space: pre-wrap;"><strong>芯片类型 : </strong>${hardware_capability["chip_type"]}  </span>`;
	html += `<span style="white-space: pre-wrap;"><strong>软件版本 : </strong>${solutions_config["version"]}  </span>`;
	html += `<span style="white-space: pre-wrap;"><strong>码流链接 : </strong>rtsp://${window.location.host}/stream_chn0.h264</span>`;

	html += `<h2>选择应用方案</h2>`
	html += `<form id="solutionForm">`
	html += `<label for="cam_solution">
				<input type="radio" id="cam_solution" name="solution" value="cam_solution"
					${solution_name === "cam_solution" ? 'checked' : ''}> 智能摄像机
				<a href="#" class="info-icon" onclick="open_solution_info('cam_solution')">?</a>
			</label>`;
	html += `<label for="box_solution">
				<input type="radio" id="box_solution" name="solution" value="box_solution"
					${solution_name === "box_solution" ? 'checked' : ''}> 智能分析盒
				<a href="#" class="info-icon" onclick="open_solution_info('box_solution')">?</a>
			</label>`;
	html += `</form>`;

	if (solution_name === 'cam_solution') {
		const cam_solution = solutions_config["cam_solution"];
		g_current_layout = cam_solution["pipeline_count"];

		// 渲染 cam_vpp
		html += `<div><strong>智能摄像机</strong><ul>`;

		// 渲染 pipeline_count 下拉选择框
		const field = g_solution_fields["pipeline_count"];
		const label = field ? `${field.chinese_name}（pipeline_count）` : "pipeline_count";
		html += `<div"><span>${label}</span>：<select id="item_cam_pipeline_count" class="form-control-sm">`;
		for (let option = 1; option <= cam_solution[field.options]; option++) {
			html += `<option value="${option}" ${cam_solution["pipeline_count"] === option ? 'selected' : ''}>${option}</option>`;
		}
		html += `</select></div>`;

		for (let i = 0; i < cam_solution["pipeline_count"]; i++) {
			html += `<li style="display: inline-block;"><strong>第 ${i+1} 路配置：</strong><ul>`;
			for (const itemKey in cam_solution["cam_vpp"][i]) {
				const uniqueId = `item_${i}_${itemKey}`;
				html += render_label_name(solutions_config, itemKey, uniqueId, cam_solution["cam_vpp"][i]);
			}
			html += `</ul></li>`;
		}
		html += `</ul></div>`;

		container.innerHTML = html;

		// 添加事件监听器到 pipeline_count 下拉选择框
		document.getElementById("item_cam_pipeline_count").addEventListener("change", function () {
			const selectedPipelineCount = parseInt(this.value);
			// 根据选中的 pipeline_count 更新 cam_vpp 的显示
			g_solution_configs["cam_solution"]["pipeline_count"] = selectedPipelineCount;
			render_json_to_html(g_solution_configs);
		});
	} else if (solution_name === 'box_solution') {
		const box_solution = solutions_config["box_solution"];
		g_current_layout = box_solution["pipeline_count"];

		// 渲染 box_vpp
		html += `<div><strong>智能分析盒</strong><ul>`;
		// 渲染 pipeline_count 下拉选择框
		const field = g_solution_fields["pipeline_count"];
		const label = field ? `${field.chinese_name}（pipeline_count）` : "pipeline_count";
		html += `<div"><span>${label}</span>：<select id="item_box_pipeline_count" class="form-control-sm">`;
		for (let option = 1; option <= box_solution[field.options]; option++) {
			html += `<option value="${option}" ${box_solution["pipeline_count"] === option ? 'selected' : ''}>${option}</option>`;
		}
		html += `</select></div>`;

		for (let i = 0; i < box_solution["pipeline_count"]; i++) {
			html += `<li style="display: inline-block;"><strong>第 ${i+1} 路配置：</strong><ul>`;
			for (const itemKey in box_solution["box_vpp"][i]) {
				const uniqueId = `item_${i}_${itemKey}`;
				html += render_label_name(solutions_config, itemKey, uniqueId, box_solution["box_vpp"][i]);
			}
			html += `</ul></li>`;
		}
		html += `</ul></div>`;
		container.innerHTML = html;
		// 添加事件监听器到 pipeline_count 下拉选择框
		document.getElementById("item_box_pipeline_count").addEventListener("change", function () {
			const selectedPipelineCount = parseInt(this.value);
			// 根据选中的 pipeline_count 更新 box_vpp 的显示
			g_solution_configs["box_solution"]["pipeline_count"] = selectedPipelineCount;
			render_json_to_html(g_solution_configs);
		});
	}

	// 调整视频显示格
	adjust_layout(g_current_layout); // 根据实际情况调整参数
	update_solution_status(solutions_config);

	// 添加事件监听器到应用方案选择
	document.getElementById('cam_solution').addEventListener('click', function () {
		const selectedSolution = this.value;
		// 更新选中的解决方案名称
		g_solution_configs["solution_name"] = selectedSolution;
		var solution_image = document.getElementById("solution_image");
		solution_image.style.display = "block";
		solution_image.setAttribute("src", "image/camera-slt.jpg");
		// 重新渲染页面
		render_json_to_html(g_solution_configs);
	});

	document.getElementById('box_solution').addEventListener('click', function () {
		const selectedSolution = this.value;
		// 更新选中的解决方案名称
		g_solution_configs["solution_name"] = selectedSolution;
		var solution_image = document.getElementById("solution_image");
		solution_image.style.display = "block";
		solution_image.setAttribute("src", "image/box-slt.jpg");
		// 重新渲染页面
		render_json_to_html(g_solution_configs);
	});
}

// 页面加载后立即请求连接 websocket 服务器
// 连接成功后，同步时间、获取能力集、调整UI、拉流
var serverIp = window.location.host;
$(document).ready(function () {
	// alert("serverIp:" + serverIp);
	// 链接 websocket 服务器
	// 发起连接
	socket.init("ws://" + serverIp + ":4567")
});

function ws_send_cmd(kind, data) {
	var cmd = {
		kind: kind,
		param: data
	};
	console.log(cmd);
	socket.send(cmd);
}
// 示例用法：
// ws_send_cmd(REQUEST_TYPES.SET_ENCODE_BITRATE, Number(data)); // 设置编码码率

function draw_detection_result(pipeline, detection_result) {
	//获取画布DOM  还不可以操作

	// 原图的大小是 1920 * 1080 或者 3840 * 2160
	// web上显示的大小不一定是这个值，所以画框的时候需要做比例调整
	// 获取真实视频的分辨率
	// 获取与canvas关联的video对象，并且把  canvas 的 width 和 height 设置为 video 的 width 和 height
	var video = document.getElementById(`video${g_current_layout}_${pipeline}`);

	var canvas = document.getElementById(`canvas${g_current_layout}_${pipeline}`);
	canvas.width = video.videoWidth;
	canvas.height = video.videoHeight;

	var context2D = canvas.getContext("2d");

	// 清空
	context2D.clearRect(0, 0, canvas.width, canvas.height);
	context2D.globalAlpha = 50;

	// 遍历bbox
	context2D.lineWidth = 2;
	context2D.strokeStyle = "#f1af37";
	context2D.font = "24px Arial";
	context2D.fillStyle = "#ff6666";    // 柔和浅红色
	for (var i in detection_result) {
		/*console.log(detection_result[i]);*/
		var result = detection_result[i];
		context2D.strokeRect(result.bbox[0], result.bbox[1], (result.bbox[2] - result.bbox[0]) , (result.bbox[3] - result.bbox[1]));
		if (result.name) {
			// 如果 result.name 存在，则执行以下逻辑
			context2D.fillText(result.name + "(" + result.score + ")", result.bbox[0], result.bbox[1]);
		} else if (result.class_name) {
			// 如果 result.name 不存在但 result.class_name 存在，则执行以下逻辑
			context2D.fillText(result.class_name + "(" + result.prob + ")", result.bbox[0], result.bbox[1]);
		}
	}
	context2D.stroke();
	context2D.fill();
}

function show_classification_result(pipeline, msg) {
	const alogResultId = `alog_result${g_current_layout}_${pipeline}`;
	// 获取对应的 overlay 元素
	const alogResultOverlay = document.getElementById(alogResultId);
	// 填写 overlay 的值
	if (alogResultOverlay) {
		// 解析 msg 获取 id
		const idMatch = msg.match(/id=(\d+)/);
		if (idMatch && idMatch[1]) {
			const id = idMatch[1];
			// 调用 get_class_name_by_id 获取 class_name
			const class_name = get_class_name_by_id(String(id));
			// 如果成功获取到 class_name，则补充到 msg 后面
			if (class_name) {
				msg += `, class_name=${class_name}`;
			}
		}
		alogResultOverlay.textContent = "分类算法结果: " + msg;
	}
}

setInterval(() => {
	for (let idx = 1; idx <= g_current_layout; idx++) {
		// 清除画布上的算法渲染信息
		var canvas = document.getElementById(`canvas${g_current_layout}_${idx}`);
		var context2D = canvas.getContext("2d");
		context2D.clearRect(0, 0, canvas.width, canvas.height);
		context2D.globalAlpha = 50;

		// 构造对应方格的 id
		const alogResultId = `alog_result${g_current_layout}_${idx}`;
		// 获取对应的 overlay 元素
		const alogResultOverlay = document.getElementById(alogResultId);
		// 填写 overlay 的值
		if (alogResultOverlay) {
			// 在这里填写你要显示的内容，例如：
			alogResultOverlay.textContent = "";
		}
	}
}, 1000);

setInterval(() => {
	for (let idx = 1; idx <= g_current_layout; idx++) {
		// 构造对应方格的 id
		const overlayId = `status${g_current_layout}_${idx}`;

		// 获取对应的 overlay 元素
		const statusOverlay = document.getElementById(overlayId);

		// 填写 overlay 的值
		if (statusOverlay) {
			statusOverlay.textContent = "视频帧率: " + socket.play_fps[idx] + "   " + "算法帧率: " + socket.smart_fps[idx];
		}

		// 清零播放帧率和算法帧率
		socket.play_fps[idx] = 0;
		socket.smart_fps[idx] = 0;
	}
}, 1000); //1秒刷新一次帧率

function start_stream(chn_count) {
	console.log("start_stream, initialize the queue of algorithm results");
	g_alog_result_queue_array = [];
	for (var i = 1; i <= chn_count; i++) {
		var video = document.getElementById(`video${chn_count}_${i}`);
		var wfs = new Wfs();
		wfs.attachMedia(video, `video${chn_count}_${i}`);
		socket.wfs_handle[i] = wfs;
		// 准备算法结果的队列
		g_alog_result_queue_array[i] = [];
	}
	ws_send_cmd(REQUEST_TYPES.START_STREAM, Number(chn_count)); // 开始推流
}

function stop_stream(chn_count) {
	console.log("stop stream");

	for (var i = 1; i <= chn_count; i++) {
		// 退出拉流
		if (socket.wfs_handle[i]) {
			socket.wfs_handle[i].destroy();
		}
		socket.stream_first_timestamp[i] = -1;
		// 清空算法结果的队列，避免旧数据影响
		g_alog_result_queue_array[i] = [];
	}

	ws_send_cmd(REQUEST_TYPES.STOP_STREAM, Number(chn_count)); // 停止推流
}

// 当视图页面不是激活状态时，退出拉流
// 当再次切回来时，重新拉流
document.addEventListener("visibilitychange", () => {
	console.log("visibilitychange: " + document.hidden);
	if (document.hidden) {
		stop_stream(g_current_layout);
	} else {
		start_stream(g_current_layout);
	}
});

function downloadFile(file_path) {
	try {
		var pos = file_path.lastIndexOf('/');//'/所在的最后位置'
		var file_name = file_path.substr(pos + 1)//截取文件名称字符串
		var urlFile = "http://" + serverIp + "/tmp_file/" + file_name;
		console.log('下载文件:' + urlFile)
		var elemIF = document.createElement("iframe");
		elemIF.src = urlFile;
		elemIF.style.display = "none";
		document.body.appendChild(elemIF);
	} catch (e) {
		console.log('下载文件失败')
	}
}

function show_app_status(msg) {
}

var is_solution_configs_show = 0;
function show_solution_configs() {
	var solution_configs = document.getElementById("solution_configs");
	if (is_solution_configs_show == 0)
		solution_configs.style.display = "table";
	else
		solution_configs.style.display = "none";
	is_solution_configs_show = is_solution_configs_show ? 0 : 1;
}

function update_json_from_html() {
	// 更新 solution_name
	const solution_name = document.querySelector('input[name="solution"]:checked').value;
	g_solution_configs["solution_name"] = solution_name;

	// 更新 cam_solution 或 box_solution 的内容
	if (solution_name === 'cam_solution') {
		const cam_solution = g_solution_configs["cam_solution"];
		// 更新 pipeline_count
		const pipelineCountSelect = document.getElementById("item_cam_pipeline_count");
		cam_solution["pipeline_count"] = parseInt(pipelineCountSelect.value);

		// 更新 cam_vpp
		for (let i = 0; i < cam_solution["pipeline_count"]; i++) {
			for (const itemKey in cam_solution["cam_vpp"][i]) {
				const uniqueId = `item_${i}_${itemKey}`;
				const element = document.getElementById(uniqueId);
				if (element.tagName === "INPUT") {
					// 根据输入元素的类型更新字段值
					if (element.type === "number") {
						cam_solution["cam_vpp"][i][itemKey] = parseInt(element.value);
					} else {
						cam_solution["cam_vpp"][i][itemKey] = element.value;
					}
				} else if (element.tagName === "SELECT") {
					// 获取选中的选项索引
					const selectedIndex = element.selectedIndex;
					const selectedValue = element.value.trim();
					// 检查选择的值是否是一个有效的数字
					const numericValue = parseInt(selectedValue);
					if (!isNaN(numericValue)) {
						// 如果是数字，则按照数字处理
						// 更新 JSON 中对应字段的值
						cam_solution["cam_vpp"][i][itemKey] = numericValue;
					} else {
						// 如果不是数字，则按照字符串处理
						// 对于 encode_type 或 decode_type，读取下拉选择框的编号值
						if (itemKey === 'encode_type' || itemKey === 'decode_type') {
							cam_solution["cam_vpp"][i][itemKey] = parseInt(selectedIndex);
						} else {
							// 对于其他字段，直接更新为选择的文本值
							cam_solution["cam_vpp"][i][itemKey] = selectedValue;
						}
					}
				}
			}
		}
	} else if (solution_name === 'box_solution') {
		const box_solution = g_solution_configs["box_solution"];
		// 更新 pipeline_count
		const pipelineCountSelect = document.getElementById("item_box_pipeline_count");
		box_solution["pipeline_count"] = parseInt(pipelineCountSelect.value);

		// 更新 box_vpp
		for (let i = 0; i < box_solution["pipeline_count"]; i++) {
			for (const itemKey in box_solution["box_vpp"][i]) {
				const uniqueId = `item_${i}_${itemKey}`;
				const element = document.getElementById(uniqueId);
				if (element.tagName === "INPUT") {
					// 根据输入元素的类型更新字段值
					if (element.type === "number") {
						box_solution["box_vpp"][i][itemKey] = parseInt(element.value);
					} else {
						box_solution["box_vpp"][i][itemKey] = element.value;
					}
				} else if (element.tagName === "SELECT") {
					// 获取选中的选项索引
					const selectedIndex = element.selectedIndex;
					const selectedValue = element.value.trim();
					// 检查选择的值是否是一个有效的数字
					const numericValue = parseInt(selectedValue);
					if (!isNaN(numericValue)) {
						// 如果是数字，则按照数字处理
						// 更新 JSON 中对应字段的值
						box_solution["box_vpp"][i][itemKey] = numericValue;
					} else {
						// 如果不是数字，则按照字符串处理
						// 对于 encode_type 或 decode_type，读取下拉选择框的编号值
						if (itemKey === 'encode_type' || itemKey === 'decode_type') {
							box_solution["box_vpp"][i][itemKey] = parseInt(selectedIndex);
						} else {
							// 对于其他字段，直接更新为选择的文本值
							box_solution["box_vpp"][i][itemKey] = selectedValue;
						}
					}
				}
			}
		}
	}

	// console.log(g_solution_configs);
	update_solution_status(g_solution_configs);
}


function switch_solution() {
	stop_stream(g_current_layout);

	update_json_from_html();

	ws_send_cmd(REQUEST_TYPES.APP_SWITCH, JSON.stringify(g_solution_configs)); // 应用切换
}

function save_solution_configs() {
	update_json_from_html();

	ws_send_cmd(REQUEST_TYPES.SAVE_CONFIGS, JSON.stringify(g_solution_configs)); // 保存配置
}

function recovery_solution_configs() {
	ws_send_cmd(REQUEST_TYPES.RECOVERY_CONFIGS); // 恢复配置
}

function get_raw_frame() {
	ws_send_cmd(REQUEST_TYPES.SNAPSHOT, 'raw'); // 抓拍
}

function get_yuv_frame() {
	ws_send_cmd(REQUEST_TYPES.SNAPSHOT, 'yuv'); // 抓拍
}

function open_video() {
	if (navigator.mediaDevices === undefined) {
		navigator.mediaDevices = {};
	}
	//
	if (navigator.mediaDevices.getUserMedia === undefined) {
		navigator.mediaDevices.getUserMedia = function (constraints) {
			var getUserMedia = navigator.webkitGetUserMedia || navigator.mozGetUserMedia;
			if (!getUserMedia) {
				return Promise.reject(new Error('getUserMedia is not implemented in this browser'));
			}
			return new Promise(function (resolve, reject) {
				getUserMedia.call(navigator, constraints, resolve, reject);
			});
		}
	}

	window.URL = (window.URL || window.webkitURL || window.mozURL || window.msURL);
	var mediaOpts = {
		audio: false,
		video: true,
	}
	function errorFunc(err) {
		alert(err.name);
	}

	navigator.mediaDevices.getUserMedia(mediaOpts, successFunc, errorFunc);
}

// websocket 连接成功
function ws_onopen() {
	// sdb 板子上没有rtc保存时间，所以把pc的时间同步到设备上，让osd时间和pc时间同步
	var currentTime = new Date().getTime() / 1000;
	console.log("currentTime:", currentTime);
	ws_send_cmd(REQUEST_TYPES.SYNC_TIME, Number(currentTime)); // 时间同步

	// 请求类型 kind 7 获取设备信息，如软件版本、芯片类型等
	// 之后接收到设备能力信息后，根据能力集信息进行UI显示调整
	ws_send_cmd(REQUEST_TYPES.GET_CONFIG); // 获取配置
}

function handle_ws_recv(params) {
	// console.log(params);
	if (params.kind == REQUEST_TYPES.APP_SWITCH && params.Status == 200) {
		if (Wfs.isSupported()) {
			start_stream(g_current_layout);
		}
	} else if (params.kind == REQUEST_TYPES.APP_SWITCH && params.app_status) {
		show_app_status(params.app_status);
	} else if (params.kind == REQUEST_TYPES.SNAPSHOT) {
		downloadFile(params.Filename);
	} else if (params.kind == REQUEST_TYPES.ALOG_RESULT) {
		// console.log("Input", params);
		if (params.classification_result) {
			socket.smart_fps[params.pipeline]++;
		}
		if (params.detection_result) {
			socket.smart_fps[params.pipeline]++;
		}
		if (!g_alog_result_queue_array[params.pipeline]) {
			g_alog_result_queue_array[params.pipeline] = []; // 如果不存在，创建一个空数组
		}
		// 将 params 放入相应的队列中
		g_alog_result_queue_array[params.pipeline].push(params);
	} else if (params.kind == REQUEST_TYPES.GET_CONFIG) {
		if (params.solution_configs) {
			g_solution_configs = params.solution_configs;
			render_json_to_html(g_solution_configs);
		}
		// 完成UI界面渲染和调整后再拉流
		// 保证首次拉流时websocket已经连接
		if (Wfs.isSupported()) {
			start_stream(g_current_layout);
		}
	}
}
