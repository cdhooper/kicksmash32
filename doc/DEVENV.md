The easiest way to build the software is to use VSCode with the pre-configured devcontainer. You will however need a working Docker daemon and Git installation in order to clone the repo and run the container image.

> [!NOTE]
> if you are not familiar with VSCode and devcontainers please read [Developing inside a Container](https://code.visualstudio.com/docs/devcontainers/containers)

There are now just two steps to get the devcontainer up and running.

1. Clone the repo using VSCode. ![clone repo](vscode_clone.png)
2. Click `reopen in container` from the notification. ![reopen in container](reopen_in_container.png)

> [!TIP]
>It may take some time to build/download the container image the first time you click `reopen in container` just be patient.

Once these two steps are complete, you can open a new terminal in VSCode (`ctrl` + `shift` + `'`). This terminal will be attached to the Fedora devcontainer.

To build the software, simply press `ctrl` + `shift` + `b`, or alternatively select `Run Build Task` from the `Termianl` Menu.

This will invoke `Make All` inside the devcontainer and the root directory of the repo.
