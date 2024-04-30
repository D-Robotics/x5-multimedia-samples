import cv2
import numpy as np
 
img = cv2.imread("../../images/image1280x960.jpg")
 
cameraMatrix = np.zeros((3,3), dtype=np.float32)
cameraMatrix[0,0] = 682 #fx
cameraMatrix[0,2] = 960 #cx
cameraMatrix[1,1] = 682 #fy
cameraMatrix[1,2] = 540 #cy
cameraMatrix[2,2] = 1
 
distCoeffs = np.array([0.0000, 0.00000, 0.000, 0.0, 0.0]) #k1 k2 pa p2 k3
h, w = img.shape[:2]
print(img.shape)
newCameraMatrix, roi = cv2.getOptimalNewCameraMatrix(cameraMatrix, distCoeffs, (w,h), 1, (w,h))
mapx, mapy = cv2.initUndistortRectifyMap(cameraMatrix, distCoeffs, None, newCameraMatrix, (w,h), cv2.CV_32FC1)
dst = cv2.remap(img, mapx, mapy, cv2.INTER_NEAREST)
# Output custom config
with open('custom_config.txt', 'w') as f:
    f.write(str(1)+"\n")
    f.write(str(50) + " " + str(50) + "\n")
    f.write(str(h) + " " + str(w) + "\n")
    f.write(str(h/2 - 1) + " " + str(w/2 - 1) + "\n")
    for i in range(0, h):
        for j in range(0, w):
            if mapy[i,j] < 0:
                mapy[i,j] = 0
            if mapx[i,j] < 0:
                mapx[i,j] = 0
            f.write(str(mapy[i,j]) + ":" + str(mapx[i,j]) + " ")
        f.write("\n")
cv2.imshow("test", dst)
cv2.waitKey(0)
cv2.destroyAllWindows()