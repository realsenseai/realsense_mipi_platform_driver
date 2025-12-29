import org.jenkinsci.plugins.workflow.steps.FlowInterruptedException

pipeline {
	agent { label 'rs-orin-01.realsenseai.com' }

	options {
		timestamps()
			timeout(time: 30, unit: 'MINUTES')
	}

	parameters {
		booleanParam(name: 'REBOOT', defaultValue: false)
	}

	stages {
		stage('Get artifacts') {
			when {
				expression { params.REBOOT == true }
			}
			steps {
				script {
					copyArtifacts filter: '**/*.tar.bz2',
						      projectName: 'D4xx_Kernel_Module_Jetson_JP6',
						      flatten: true,
						      target: 'artifacts/'
				}
			}
		}
		stage('Install artifacts') {
			when {
				expression { params.REBOOT == true }
			}
			steps {
				sh """#!/bin/sh
					tar -xf artifacts/rootfs.tar.bz2
					# external script on agent to install artifacts
					sudo install.tegra.artifacts.sh
				"""
				script {
					build job: env.JOB_NAME,
					      wait: false,
					      parameters: [ booleanParam(name: 'REBOOT', value: false) ]
				}
			}
			post {
				success {
					sh 'nohup sudo reboot &'
				}
			}
		}
		stage('Pytest') {
			when {
				expression { params.REBOOT == false }
			}
			steps {
				sh """#!/bin/sh
					pytest --tb=no -s test
				"""
			}
		}
	}
}
