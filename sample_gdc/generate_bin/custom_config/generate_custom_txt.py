import os
import cv2
import numpy as np

# Read the input image
img = cv2.imread("../../images/image1280x960.jpg")

# Define the camera matrix with initial values
cameraMatrix = np.zeros((3,3), dtype=np.float32)
cameraMatrix[0,0] = 640 # focal length in x-direction (fx)
cameraMatrix[0,2] = 960 # principal point in x-direction (cx)
cameraMatrix[1,1] = 480 # focal length in y-direction (fy)
cameraMatrix[1,2] = 540 # principal point in y-direction (cy)
cameraMatrix[2,2] = 1   # set the bottom-right value to 1

# The distortion coefficients are used to model the radial and tangential distortion present in the camera lens.
# The radial distortion is primarily caused by the lens shape and results in a curvature effect in the image.
# - k1 and k2 are the radial distortion coefficients.
#   - k1 represents the primary radial distortion term.
#   - k2 represents the secondary radial distortion term.
#   - These values are typically negative for barrel distortion and positive for pincushion distortion.
# - p1 and p2 are the tangential distortion coefficients.
#   - They account for the slight off-centeredness of the lens components.
#   - These values are usually close to zero unless there are lens misalignments.
# - k3 is an additional radial distortion coefficient, typically kept at zero unless higher-order distortion is present.
#   - Higher-order distortion effects are generally less common and often not needed to be considered.
# These initial values (-0.01, -0.01, 0.0, 0.0, 0.0) are estimated based on the observed distortion characteristics
# of the camera lens and can be further refined through camera calibration techniques using calibration images.
# The calibration process involves capturing images of known calibration patterns (e.g., chessboard) from different
# angles and distances, and then iteratively adjusting the distortion coefficients to minimize the difference between
# the observed and expected positions of calibration points.
distCoeffs = np.array([-0.01, -0.01, 0.0, 0.0, 0.0])  # (k1, k2, p1, p2, k3)

# Get the height and width of the input image
h, w = img.shape[:2]

# Get the optimal new camera matrix and region of interest (ROI)
newCameraMatrix, roi = cv2.getOptimalNewCameraMatrix(cameraMatrix, distCoeffs, (w,h), 0, (w,h))

# Initialize the undistortion map
mapx, mapy = cv2.initUndistortRectifyMap(cameraMatrix, distCoeffs, None, newCameraMatrix, (w,h), cv2.CV_32FC1)

# Remap the distorted image to undistorted image
dst = cv2.remap(img, mapx, mapy, cv2.INTER_NEAREST)

# Output custom configuration to a text file
with open('custom_config.txt', 'w') as f:
    f.write(str(1)+"\n") # Write some parameters
    f.write(str(50) + " " + str(50) + "\n") # Write some parameters
    f.write(str(h) + " " + str(w) + "\n") # Write height and width of the image
    f.write(str(h/2 - 1) + " " + str(w/2 - 1) + "\n") # Write the center coordinates
    for i in range(0, h):
        for j in range(0, w):
            if mapy[i,j] < 0:
                mapy[i,j] = 0
            if mapx[i,j] < 0:
                mapx[i,j] = 0
            f.write(str(mapy[i,j]) + ":" + str(mapx[i,j]) + " ") # Write the remapped coordinates
        f.write("\n")

print("Done")

# Display the original image if a graphical environment is available
if os.environ.get('DISPLAY'):
    # Concatenate the two images horizontally into one large image
    combined_img = np.hstack((img, dst))

    # Create a window and set size limits
    cv2.namedWindow("Comparison", cv2.WINDOW_NORMAL)
    cv2.resizeWindow("Comparison", 960, 320)

    # Display the combined image
    cv2.imshow("Comparison", combined_img)
    cv2.waitKey(0)
    cv2.destroyAllWindows()
